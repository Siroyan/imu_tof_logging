#include "imu_recorder.h"

#include "binary_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
#define IMU_TIMESTAMP_HZ       19200000ULL
#define IMU_STREAM_BUFFER_SAMPLES 1024

typedef struct imu_log_header_s
{
  char magic[8];
  uint32_t version;
  uint32_t sample_rate_hz;
  uint32_t record_size;
  uint32_t reserved;
  uint64_t session_start_us;
} imu_log_header_t;

/* CLOCK_MONOTONIC を us 単位に変換する。 */
static uint64_t imu_recorder_monotonic_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/*
 * IMU のハードウェア timestamp 差分を us 単位へ変換する。
 * timestamp は 32bit カウンタのため、uint32_t 差分で1周分の折り返しを吸収する。
 */
static uint64_t imu_recorder_timestamp_delta_us(uint32_t timestamp,
                                                uint32_t base_timestamp)
{
  uint32_t delta = timestamp - base_timestamp;

  return ((uint64_t)delta * 1000000ULL + IMU_TIMESTAMP_HZ / 2ULL) /
         IMU_TIMESTAMP_HZ;
}

/*
 * 共通収録開始時刻を原点にした IMU サンプル時刻を作る。
 * 初回サンプルだけ CLOCK_MONOTONIC で共通時刻系へ対応付け、以降は
 * read() した時刻ではなく IMU のハードウェア timestamp 差分から復元する。
 */
static uint64_t imu_recorder_session_time_us(imu_recorder_t *recorder,
                                             uint32_t timestamp,
                                             uint64_t read_ready_us)
{
  if (!recorder->timestamp_base_valid)
    {
      recorder->timestamp_base = timestamp;
      if (read_ready_us < recorder->session_start_us)
        {
          recorder->timestamp_base_session_us = 0;
        }
      else
        {
          recorder->timestamp_base_session_us =
            read_ready_us - recorder->session_start_us;
        }
      recorder->timestamp_base_valid = 1;
      return recorder->timestamp_base_session_us;
    }

  return recorder->timestamp_base_session_us +
         imu_recorder_timestamp_delta_us(timestamp, recorder->timestamp_base);
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
  recorder->timestamp_base_session_us = 0;
  recorder->timestamp_base = 0;
  recorder->timestamp_base_valid = 0;
  return binary_file_write(recorder->fp, &header, sizeof(header), 1,
                           "IMU binary header");
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
 * IMU は2面バッファで受け、満杯になった面からSDへ非同期に書き出す。
 */
int imu_recorder_open(imu_recorder_t *recorder, int capture_seconds)
{
  (void)capture_seconds;

  recorder->fd = -1;
  recorder->fp = NULL;
  memset(&recorder->stream, 0, sizeof(recorder->stream));
  recorder->capacity = IMU_STREAM_BUFFER_SAMPLES;
  recorder->total = 0;
  recorder->session_start_us = 0;
  recorder->timestamp_base_session_us = 0;
  recorder->timestamp_base = 0;
  recorder->timestamp_base_valid = 0;
  recorder->failed = 0;

  recorder->fd = open(CXD5602PWBIMU_DEVPATH, O_RDONLY);
  if (recorder->fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n",
             CXD5602PWBIMU_DEVPATH, errno);
      recorder->failed = 1;
      return 1;
    }

  recorder->fp = binary_file_open(IMU_LOG_PATH);
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
      return 1;
    }

  if (binary_stream_open(&recorder->stream,
                         recorder->fp,
                         sizeof(imu_sample_t),
                         recorder->capacity,
                         "IMU samples"))
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
 * active 面が満杯の場合は writer thread へ渡し、空き面へ切り替えてから格納する。
 */
int imu_recorder_read_ready(imu_recorder_t *recorder)
{
  int ret;
  uint64_t read_ready_us;
  imu_sample_t sample;

  read_ready_us = imu_recorder_monotonic_us();
  ret = read(recorder->fd, &sample.data, sizeof(sample.data));
  if (ret != sizeof(sample.data))
    {
      printf("ERROR: read size mismatch! %d\n", ret);
      return 0;
    }

  sample.session_time_us =
    imu_recorder_session_time_us(recorder,
                                 sample.data.timestamp,
                                 read_ready_us);
  if (binary_stream_append(&recorder->stream, &sample))
    {
      recorder->failed = 1;
      return -1;
    }

  recorder->total++;
  return 1;
}

int imu_recorder_buffer_count(const imu_recorder_t *recorder)
{
  return binary_stream_count(&recorder->stream);
}

/*
 * 残った IMU サンプルを書き出し、バイナリ、デバイス、RAMバッファを解放する。
 * 戻り値は保存または後始末で失敗があったかどうかを示す。
 */
int imu_recorder_finish(imu_recorder_t *recorder)
{
  if (recorder->fp != NULL && binary_stream_close(&recorder->stream))
    {
      recorder->failed = 1;
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
