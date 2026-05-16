#include "imu_recorder.h"

#include "csv_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CXD5602PWBIMU_DEVPATH "/dev/imu0"
#define IMU_LOG_PATH          "/mnt/sd0/imu_log.csv"
#define IMU_ACCEL_RANGE       2
#define IMU_GYRO_RANGE        125
#define IMU_FIFO_THRESHOLD    1

/*
 * RAM に蓄積した IMU サンプルを CSV に書き出す。
 * 収録中の SD 書き込みを極力減らすため、通常は終了処理でまとめて呼ばれる。
 */
static int imu_recorder_flush(imu_recorder_t *recorder)
{
  cxd5602pwbimu_data_t *p;

  if (recorder->count == 0)
    {
      return 0;
    }

  /* IMU timestamp は 19.2MHz カウンタなので、秒単位へ変換して保存する。 */
  for (p = recorder->buffer; p < recorder->buffer + recorder->count; p++)
    {
      if (fprintf(recorder->fp,
                  "%.6f,%.6f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                  p->timestamp / 19200000.0f,
                  p->temp,
                  p->gx, p->gy, p->gz,
                  p->ax, p->ay, p->az) < 0)
        {
          printf("ERROR: Failed to write IMU data. %d\n", errno);
          recorder->failed = 1;
          return 1;
        }
    }

  if (fflush(recorder->fp) != 0)
    {
      printf("ERROR: Failed to flush IMU data to SD. %d\n", errno);
      recorder->failed = 1;
      return 1;
    }

  recorder->count = 0;
  return 0;
}

/*
 * CXD5602PWBIMU のサンプリング条件を設定して計測を開始する。
 * レート、レンジ、FIFOしきい値はこのファイル内の定数で固定する。
 */
static int imu_start_sensing(int fd)
{
  cxd5602pwbimu_range_t range;

  if (ioctl(fd, SNIOC_SSAMPRATE, IMU_RECORDER_SAMPLE_RATE_HZ))
    {
      printf("ERROR: Set sampling rate failed. %d\n", errno);
      return 1;
    }

  range.accel = IMU_ACCEL_RANGE;
  range.gyro = IMU_GYRO_RANGE;
  if (ioctl(fd, SNIOC_SDRANGE, (unsigned long)(uintptr_t)&range))
    {
      printf("ERROR: Set dynamic range failed. %d\n", errno);
      return 1;
    }

  if (ioctl(fd, SNIOC_SFIFOTHRESH, IMU_FIFO_THRESHOLD))
    {
      printf("ERROR: Set FIFO threshold failed. %d\n", errno);
      return 1;
    }

  if (ioctl(fd, SNIOC_ENABLE, 1))
    {
      printf("ERROR: Enable failed. %d\n", errno);
      return 1;
    }

  return 0;
}

/*
 * IMU recorder のデバイス、出力CSV、RAMバッファを準備する。
 * バッファは指定秒数分を目標に確保し、足りない場合は半分ずつ下げて確保を試す。
 */
int imu_recorder_open(imu_recorder_t *recorder, int capture_seconds)
{
  int target_samples;

  recorder->fd = -1;
  recorder->fp = NULL;
  recorder->buffer = NULL;
  recorder->capacity = 0;
  recorder->count = 0;
  recorder->total = 0;
  recorder->failed = 0;

  recorder->fd = open(CXD5602PWBIMU_DEVPATH, O_RDONLY);
  if (recorder->fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n",
             CXD5602PWBIMU_DEVPATH, errno);
      recorder->failed = 1;
      return 1;
    }

  /* まず収録時間全体を保持できる容量を狙い、確保できなければ縮小する。 */
  target_samples = IMU_RECORDER_SAMPLE_RATE_HZ * capture_seconds;
  for (recorder->capacity = target_samples;
       recorder->capacity >= 1;
       recorder->capacity /= 2)
    {
      recorder->buffer = (cxd5602pwbimu_data_t *)
        malloc(sizeof(cxd5602pwbimu_data_t) * recorder->capacity);
      if (recorder->buffer != NULL)
        {
          break;
        }
    }

  if (recorder->buffer == NULL)
    {
      printf("ERROR: Output buffer allocation failed.\n");
      recorder->failed = 1;
      return 1;
    }

  recorder->fp = csv_file_open(IMU_LOG_PATH, "timestamp,temp,gx,gy,gz,ax,ay,az");
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
    }

  return recorder->fp == NULL ? 1 : 0;
}

/* IMU のサンプリングを開始する。 */
int imu_recorder_start(imu_recorder_t *recorder)
{
  return imu_start_sensing(recorder->fd);
}

/* IMU のサンプリングを停止する。終了経路では失敗しても後始末を続ける。 */
void imu_recorder_stop(imu_recorder_t *recorder)
{
  if (recorder->fd >= 0)
    {
      ioctl(recorder->fd, SNIOC_ENABLE, 0);
    }
}

/*
 * poll() で読み出し可能になった IMU サンプルを 1 件取り込む。
 * バッファが満杯の場合は CSV へ退避してから次のサンプルを格納する。
 */
int imu_recorder_read_ready(imu_recorder_t *recorder)
{
  int ret;

  if (recorder->count >= recorder->capacity && imu_recorder_flush(recorder))
    {
      return -1;
    }

  ret = read(recorder->fd, &recorder->buffer[recorder->count],
             sizeof(recorder->buffer[recorder->count]));
  if (ret != sizeof(recorder->buffer[recorder->count]))
    {
      printf("ERROR: read size mismatch! %d\n", ret);
      return 0;
    }

  recorder->count++;
  recorder->total++;
  return 1;
}

/*
 * 残った IMU サンプルを書き出し、CSV、デバイス、RAMバッファを解放する。
 * 戻り値は保存または後始末で失敗があったかどうかを示す。
 */
int imu_recorder_finish(imu_recorder_t *recorder)
{
  if (!recorder->failed && recorder->fp != NULL && recorder->count > 0)
    {
      imu_recorder_flush(recorder);
    }

  if (recorder->fp != NULL && fclose(recorder->fp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", IMU_LOG_PATH, errno);
      recorder->failed = 1;
    }

  if (recorder->fd >= 0)
    {
      close(recorder->fd);
    }

  free(recorder->buffer);

  if (!recorder->failed)
    {
      printf("Saved samples to %s while sampling.\n", IMU_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", IMU_LOG_PATH);
    }

  return recorder->failed;
}
