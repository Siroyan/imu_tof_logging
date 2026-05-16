#include "capture_session.h"

#include "adc_a5_recorder.h"
#include "imu_recorder.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_CAPTURE_SECONDS 10

/*
 * IMU と ADC A5 の両方を poll() で待つ。
 * タイムアウトは異常終了にはせず、呼び出し元のループを継続させる。
 */
static int wait_capture_events(struct pollfd *fds)
{
  int ret = poll(fds, 2, 1000);

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
 * IMU バッファの使用率を一定間隔で表示する。
 * ADC は高レートで取得するため、ここでは既存の進捗表示として IMU 側だけを見る。
 */
static void report_buffer_usage(imu_recorder_t *imu,
                                struct timespec *now,
                                struct timespec *prev)
{
  struct timespec delta;

  clock_timespec_subtract(now, prev, &delta);
  if (delta.tv_sec > 0 || delta.tv_nsec >= 250000000L)
    {
      printf("BUF %.1f%% (%d/%d)\n",
             imu->count * 100.0f / (float)imu->capacity,
             imu->count,
             imu->capacity);
      *prev = *now;
    }
}

/*
 * 収録セッション全体を管理する。
 * センサー別の初期化やCSV保存は各 recorder に任せ、この関数は起動順、
 * poll ループ、収録時間、終了処理だけを担当する。
 */
int capture_session_run(void)
{
  int ret;
  int started = 0;
  int report_started = 0;
  int capture_failed = 0;
  struct pollfd fds[2];
  struct timespec start, now, elapsed, report_prev;
  imu_recorder_t imu;
  adc_a5_recorder_t adc;

  memset(&start, 0, sizeof(start));
  memset(&now, 0, sizeof(now));
  memset(&elapsed, 0, sizeof(elapsed));
  memset(&report_prev, 0, sizeof(report_prev));

  /* 先に両 recorder のデバイス、CSV、RAMバッファを準備する。 */
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

  printf("Capture buffer: %d samples (%.3f sec)\n",
         imu.capacity,
         imu.capacity / (float)IMU_RECORDER_SAMPLE_RATE_HZ);
  printf("ADC A5 buffer: %d samples (%.3f sec)\n",
         adc.capacity,
         adc.capacity / (float)ADC_A5_RECORDER_SAMPLE_RATE_HZ);

  fds[0].fd = imu.fd;
  fds[0].events = POLLIN;
  fds[1].fd = adc.fd;
  fds[1].events = POLLIN;

  /*
   * IMU を先に開始し、その直後に ADC を開始する。
   * ADC の余分な先行サンプルを減らしつつ、両方を同じ poll ループで扱う。
   */
  if (imu_recorder_start(&imu))
    {
      capture_failed = 1;
      goto finish;
    }

  if (adc_a5_recorder_start(&adc))
    {
      capture_failed = 1;
      imu_recorder_stop(&imu);
      goto finish;
    }

  while (1)
    {
      /* どちらかのデバイスに読み出し可能データが来るまで待つ。 */
      ret = wait_capture_events(fds);
      if (ret < 0)
        {
          capture_failed = errno != EINTR;
          break;
        }
      if (ret == 0)
        {
          continue;
        }

      /*
       * ADC は 16kHz のため、読み出し可能になった時点で優先的に吸い上げる。
       * CSV 書き込みは recorder 内で必要時のみ行う。
       */
      if ((fds[1].revents & POLLIN) && adc_a5_recorder_read_ready(&adc) < 0)
        {
          capture_failed = 1;
          break;
        }

      if (fds[0].revents & POLLIN)
        {
          /* 最初の IMU サンプルを収録開始時刻の基準にする。 */
          ret = imu_recorder_read_ready(&imu);
          if (ret < 0)
            {
              capture_failed = 1;
              break;
            }
          if (ret > 0 && !started)
            {
              clock_gettime(CLOCK_MONOTONIC, &start);
              report_prev = start;
              started = 1;
              report_started = 1;
            }
        }

      if (started)
        {
          /* 収録時間は CLOCK_MONOTONIC で測り、設定秒数に達したら終了する。 */
          clock_gettime(CLOCK_MONOTONIC, &now);
          clock_timespec_subtract(&now, &start, &elapsed);

          if (report_started)
            {
              report_buffer_usage(&imu, &now, &report_prev);
            }

          if (elapsed.tv_sec >= MAX_CAPTURE_SECONDS)
            {
              break;
            }
        }
    }

  /* ループを抜けたら、まずサンプリングを止めてから残りのCSV保存に進む。 */
  adc_a5_recorder_stop(&adc);
  imu_recorder_stop(&imu);

finish:
  /* finish はファイル close とメモリ解放も兼ねるため、エラー経路でも必ず通す。 */
  capture_failed |= adc_a5_recorder_finish(&adc);
  capture_failed |= imu_recorder_finish(&imu);

  if (started)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      clock_timespec_subtract(&now, &start, &elapsed);
    }
  else
    {
      memset(&elapsed, 0, sizeof(elapsed));
    }

  printf("Elapsed %ld.%09ld seconds\n", elapsed.tv_sec, elapsed.tv_nsec);
  printf("%d samples captured\n", imu.total);
  printf("%" PRIu32 " ADC A5 samples captured\n", adc.total);
  printf("Finished.\n");

  return capture_failed ? 1 : 0;
}
