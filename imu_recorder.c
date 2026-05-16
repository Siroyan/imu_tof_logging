#include "imu_recorder.h"

#include "binary_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CXD5602PWBIMU_DEVPATH "/dev/imu0"
#define IMU_LOG_PATH          "/mnt/sd0/imu_log.bin"
#define IMU_LOG_MAGIC         "IMULOG1"
#define IMU_LOG_VERSION       2
#define IMU_ACCEL_RANGE       2
#define IMU_GYRO_RANGE        125
#define IMU_FIFO_THRESHOLD    1

typedef struct imu_log_header_s
{
  char magic[8];
  uint32_t version;
  uint32_t sample_rate_hz;
  uint32_t record_size;
  uint32_t reserved;
  uint64_t session_start_us;
} imu_log_header_t;

/* CLOCK_MONOTONIC から、共通収録開始時刻を原点にした経過usを作る。 */
static uint64_t imu_recorder_session_time_us(const imu_recorder_t *recorder)
{
  struct timespec ts;
  uint64_t now_us;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
  if (now_us < recorder->session_start_us)
    {
      return 0;
    }

  return now_us - recorder->session_start_us;
}

/*
 * IMU バイナリログの先頭ヘッダを書き込む。
 * 後段の変換ツールが共通開始時刻、サンプリングレート、レコード形式を判別できるようにする。
 */
int imu_recorder_write_header(imu_recorder_t *recorder, uint64_t session_start_us)
{
  imu_log_header_t header = {
    IMU_LOG_MAGIC,
    IMU_LOG_VERSION,
    IMU_RECORDER_SAMPLE_RATE_HZ,
    sizeof(imu_sample_t),
    0,
    session_start_us
  };

  recorder->session_start_us = session_start_us;
  return binary_file_write(recorder->fp, &header, sizeof(header), 1,
                           "IMU binary header");
}

/*
 * RAM に蓄積した IMU サンプルをバイナリで書き出す。
 * 収録中の SD 書き込みを極力減らすため、通常は終了処理でまとめて呼ばれる。
 */
static int imu_recorder_flush(imu_recorder_t *recorder)
{
  if (recorder->count == 0)
    {
      return 0;
    }

  /*
   * 共通時刻と cxd5602pwbimu_data_t を組にして連続保存する。
   * CSV化やIMU timestamp秒換算は、必要に応じて後段の変換処理で行う。
   */
  if (binary_file_write(recorder->fp,
                        recorder->buffer,
                        sizeof(recorder->buffer[0]),
                        recorder->count,
                        "IMU samples"))
    {
      recorder->failed = 1;
      return 1;
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
 * IMU recorder のデバイス、出力バイナリ、RAMバッファを準備する。
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
  recorder->session_start_us = 0;
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
      recorder->buffer = (imu_sample_t *)
        malloc(sizeof(imu_sample_t) * recorder->capacity);
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

  recorder->fp = binary_file_open(IMU_LOG_PATH);
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
      return 1;
    }

  return 0;
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
 * バッファが満杯の場合はバイナリへ退避してから次のサンプルを格納する。
 */
int imu_recorder_read_ready(imu_recorder_t *recorder)
{
  int ret;
  imu_sample_t *sample;

  if (recorder->count >= recorder->capacity && imu_recorder_flush(recorder))
    {
      return -1;
    }

  sample = &recorder->buffer[recorder->count];
  ret = read(recorder->fd, &sample->data, sizeof(sample->data));
  if (ret != sizeof(sample->data))
    {
      printf("ERROR: read size mismatch! %d\n", ret);
      return 0;
    }

  sample->session_time_us = imu_recorder_session_time_us(recorder);
  recorder->count++;
  recorder->total++;
  return 1;
}

/*
 * 残った IMU サンプルを書き出し、バイナリ、デバイス、RAMバッファを解放する。
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
      printf("Saved IMU binary samples to %s.\n", IMU_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", IMU_LOG_PATH);
    }

  return recorder->failed;
}
