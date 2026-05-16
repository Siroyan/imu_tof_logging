#include "capture_session.h"

#include "adc_a5_recorder.h"
#include "imu_recorder.h"
#include "tof_recorder.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_CAPTURE_SECONDS 30
#define ADC_DRAIN_READ_LIMIT 16

/* CLOCK_MONOTONIC の timespec を、ログヘッダ用の us 単位へ変換する。 */
static uint64_t timespec_to_us(const struct timespec *ts)
{
  return (uint64_t)ts->tv_sec * 1000000ULL + (uint64_t)ts->tv_nsec / 1000ULL;
}

/*
 * IMU を poll() で待つ。
 * HPADC1 は poll() 未対応の環境があるため、poll 対象には含めない。
 * タイムアウトは異常終了にはせず、呼び出し元のループを継続させる。
 */
static int wait_imu_event(struct pollfd *imu_fd)
{
  int ret = poll(imu_fd, 1, 1000);

  if (ret < 0 && errno != EINTR)
    {
      printf("ERROR: poll failed. %d\n", errno);
    }
  else if (ret == 0)
    {
      printf("Timeout!\n");
    }

  return ret;
}

/*
 * ADC A5 の保留サンプルをまとめて吸い上げる。
 * read() がデータなしを示した時点で現時点のFIFOは空とみなし、連続読み出しを止める。
 */
static int drain_adc_samples(adc_a5_recorder_t *adc)
{
  int i;
  int ret;

  for (i = 0; i < ADC_DRAIN_READ_LIMIT; i++)
    {
      ret = adc_a5_recorder_read_ready(adc);
      if (ret < 0)
        {
          return -1;
        }
      if (ret == 0)
        {
          return 0;
        }
    }

  return 0;
}

/*
 * IMU と ADC A5 の active 面バッファ使用率を一定間隔で表示する。
 * 2面バッファの切り替え後は0付近へ戻るため、収録進捗ではなく滞留量を見るログになる。
 */
static void report_buffer_usage(imu_recorder_t *imu,
                                adc_a5_recorder_t *adc,
                                struct timespec *now,
                                struct timespec *prev)
{
  int adc_count;
  int imu_count;
  struct timespec delta;

  clock_timespec_subtract(now, prev, &delta);
  if (delta.tv_sec > 0 || delta.tv_nsec >= 250000000L)
    {
      imu_count = imu_recorder_buffer_count(imu);
      adc_count = adc_a5_recorder_buffer_count(adc);
      printf("BUF IMU %.1f%% (%d/%d), ADC %.1f%% (%d/%d)\n",
             imu_count * 100.0f / (float)imu->capacity,
             imu_count,
             imu->capacity,
             adc_count * 100.0f / (float)adc->capacity,
             adc_count,
             adc->capacity);
      *prev = *now;
    }
}

/*
 * 収録セッション全体を管理する。
 * センサー別の初期化やバイナリ保存は各 recorder に任せ、この関数は起動順、
 * poll ループ、収録時間、終了処理だけを担当する。
 */
int capture_session_run(void)
{
  int ret;
  int started = 0;
  int capture_failed = 0;
  uint64_t session_start_us = 0;
  struct pollfd imu_fd;
  struct timespec start, now, elapsed, report_prev, capture_end;
  imu_recorder_t imu;
  adc_a5_recorder_t adc;
  tof_recorder_t tof;

  memset(&start, 0, sizeof(start));
  memset(&now, 0, sizeof(now));
  memset(&elapsed, 0, sizeof(elapsed));
  memset(&report_prev, 0, sizeof(report_prev));
  memset(&capture_end, 0, sizeof(capture_end));

  /* 先に各 recorder のデバイス、バイナリログ、RAMバッファを準備する。 */
  ret = imu_recorder_open(&imu, MAX_CAPTURE_SECONDS);
  if (ret)
    {
      imu_recorder_finish(&imu);
      return 1;
    }

  ret = adc_a5_recorder_open(&adc, MAX_CAPTURE_SECONDS);
  if (ret)
    {
      adc_a5_recorder_finish(&adc);
      imu_recorder_finish(&imu);
      return 1;
    }

  ret = tof_recorder_open(&tof, MAX_CAPTURE_SECONDS);
  if (ret)
    {
      tof_recorder_finish(&tof);
      adc_a5_recorder_finish(&adc);
      imu_recorder_finish(&imu);
      return 1;
    }

  printf("IMU stream buffer: %d samples/face (%.3f sec)\n",
         imu.capacity,
         imu.capacity / (float)IMU_RECORDER_SAMPLE_RATE_HZ);
  printf("ADC A5 stream buffer: %d samples/face (%.3f sec)\n",
         adc.capacity,
         adc.capacity / (float)ADC_A5_RECORDER_SAMPLE_RATE_HZ);
  printf("ToF stream buffer: %d samples/face (%.3f sec)\n",
         tof.capacity,
         tof.capacity / (float)TOF_RECORDER_SAMPLE_RATE_HZ);

  imu_fd.fd = imu.fd;
  imu_fd.events = POLLIN;

  /*
   * ToF と IMU を先に開始し、ADC開始直前の CLOCK_MONOTONIC を
   * 3センサ共通の収録開始時刻としてログヘッダに保存する。
   */
  if (tof_recorder_start(&tof))
    {
      capture_failed = 1;
      goto finish;
    }

  if (imu_recorder_start(&imu))
    {
      capture_failed = 1;
      tof_recorder_stop(&tof);
      goto finish;
    }

  clock_gettime(CLOCK_MONOTONIC, &start);
  session_start_us = timespec_to_us(&start);

  if (adc_a5_recorder_start(&adc))
    {
      capture_failed = 1;
      imu_recorder_stop(&imu);
      tof_recorder_stop(&tof);
      goto finish;
    }

  if (imu_recorder_write_header(&imu, session_start_us) ||
      adc_a5_recorder_write_header(&adc, session_start_us) ||
      tof_recorder_write_header(&tof, session_start_us))
    {
      capture_failed = 1;
      adc_a5_recorder_stop(&adc);
      imu_recorder_stop(&imu);
      tof_recorder_stop(&tof);
      goto finish;
    }

  report_prev = start;
  started = 1;

  while (1)
    {
      /* IMU に読み出し可能データが来るまで待つ。 */
      ret = wait_imu_event(&imu_fd);
      if (ret < 0)
        {
          capture_failed = errno != EINTR;
          break;
        }
      if (ret == 0)
        {
          if (drain_adc_samples(&adc) < 0)
            {
              capture_failed = 1;
              break;
            }
          if (tof_recorder_read_ready(&tof) < 0)
            {
              capture_failed = 1;
              break;
            }
          clock_gettime(CLOCK_MONOTONIC, &now);
          clock_timespec_subtract(&now, &start, &elapsed);
          report_buffer_usage(&imu, &adc, &now, &report_prev);
          if (elapsed.tv_sec >= MAX_CAPTURE_SECONDS)
            {
              break;
            }
          continue;
        }

      /*
       * ADC は 16kHz のため、IMUイベントごとに先に吸い上げる。
       * HPADC1 は poll() 未対応の環境があるため、非ブロッキング read() で空判定する。
       */
      if (drain_adc_samples(&adc) < 0)
        {
          capture_failed = 1;
          break;
        }

      if (tof_recorder_read_ready(&tof) < 0)
        {
          capture_failed = 1;
          break;
        }

      if (imu_fd.revents & POLLIN)
        {
          ret = imu_recorder_read_ready(&imu);
          if (ret < 0)
            {
              capture_failed = 1;
              break;
            }
        }

      if (started)
        {
          /* 収録時間は CLOCK_MONOTONIC で測り、設定秒数に達したら終了する。 */
          clock_gettime(CLOCK_MONOTONIC, &now);
          clock_timespec_subtract(&now, &start, &elapsed);

          report_buffer_usage(&imu, &adc, &now, &report_prev);

          if (elapsed.tv_sec >= MAX_CAPTURE_SECONDS)
            {
              break;
            }
        }
    }

  if (started)
    {
      clock_gettime(CLOCK_MONOTONIC, &capture_end);
      clock_timespec_subtract(&capture_end, &start, &elapsed);
    }

  /* ループを抜けたら、まずサンプリングを止めてから残りのバイナリ保存に進む。 */
  tof_recorder_stop(&tof);
  adc_a5_recorder_stop(&adc);
  imu_recorder_stop(&imu);

finish:
  /* finish はファイル close とメモリ解放も兼ねるため、エラー経路でも必ず通す。 */
  capture_failed |= tof_recorder_finish(&tof);
  capture_failed |= adc_a5_recorder_finish(&adc);
  capture_failed |= imu_recorder_finish(&imu);

  if (!started)
    {
      memset(&elapsed, 0, sizeof(elapsed));
    }

  printf("Elapsed %ld.%09ld seconds\n", elapsed.tv_sec, elapsed.tv_nsec);
  printf("%d samples captured\n", imu.total);
  printf("%" PRIu32 " ADC A5 samples captured\n", adc.total);
  printf("%" PRIu32 " ToF samples captured\n", tof.total);
  printf("Finished.\n");

  return capture_failed ? 1 : 0;
}
