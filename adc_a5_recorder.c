#include "adc_a5_recorder.h"

#include "binary_file.h"

#include <nuttx/config.h>
#include <arch/cxd56xx/adc.h>
#include <arch/cxd56xx/scu.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ADC_A5_DEVPATH           "/dev/hpadc1"
#define ADC_A5_LOG_PATH          "/mnt/sd0/adc_a5_log.bin"
#define ADC_A5_LOG_MAGIC         "ADCA5L1"
#define ADC_A5_LOG_VERSION       1
#define ADC_A5_FREQ_COEFFICIENT  7
#define ADC_A5_READ_CHUNK_SAMPLES 128
#define ADC_A5_REFERENCE_VOLTAGE 5.0f

#ifdef CONFIG_CXD56_HPADC1_FSIZE
#  define ADC_A5_FIFO_SIZE       CONFIG_CXD56_HPADC1_FSIZE
#else
#  define ADC_A5_FIFO_SIZE       64
#endif

#ifdef CONFIG_CXD56_HPADC1_INPUT_GAIN_M6DB
#  define ADC_A5_RAW_MIN         (-29362)
#  define ADC_A5_RAW_MAX         (26375)
#else
#  define ADC_A5_RAW_MIN         SHRT_MIN
#  define ADC_A5_RAW_MAX         SHRT_MAX
#endif

typedef struct adc_a5_log_header_s
{
  char magic[8];
  uint32_t version;
  uint32_t sample_rate_hz;
  uint32_t record_size;
  float reference_voltage;
  int16_t raw_min;
  int16_t raw_max;
} adc_a5_log_header_t;

/*
 * ADC A5 バイナリログの先頭ヘッダを書き込む。
 * サンプル本体は int16_t raw の連続データで、sample_index はレコード順から復元する。
 */
static int adc_a5_recorder_write_header(FILE *fp)
{
  adc_a5_log_header_t header = {
    ADC_A5_LOG_MAGIC,
    ADC_A5_LOG_VERSION,
    ADC_A5_RECORDER_SAMPLE_RATE_HZ,
    sizeof(int16_t),
    ADC_A5_REFERENCE_VOLTAGE,
    ADC_A5_RAW_MIN,
    ADC_A5_RAW_MAX
  };

  return binary_file_write(fp, &header, sizeof(header), 1, "ADC A5 binary header");
}

/*
 * 非ブロッキング read() で「まだ読み出せるADCデータがない」状態かを判定する。
 * この場合は収録失敗ではなく、呼び出し元に 0 件として返す。
 */
static int adc_a5_is_read_empty(int err)
{
#ifdef EWOULDBLOCK
  return err == EAGAIN || err == EWOULDBLOCK;
#else
  return err == EAGAIN;
#endif
}

/*
 * RAM に蓄積した ADC A5 サンプルをバイナリで書き出す。
 * sample_index はファイル内のレコード順と sample_rate_hz から後段で復元する。
 */
static int adc_a5_recorder_flush(adc_a5_recorder_t *recorder)
{
  if (recorder->count == 0)
    {
      return 0;
    }

  /* int16_t raw を連続保存し、CSV化や電圧換算は後段の変換処理へ回す。 */
  if (binary_file_write(recorder->fp,
                        recorder->buffer,
                        sizeof(recorder->buffer[0]),
                        recorder->count,
                        "ADC A5 samples"))
    {
      recorder->failed = 1;
      return 1;
    }

  if (fflush(recorder->fp) != 0)
    {
      printf("ERROR: Failed to flush ADC A5 data to SD. %d\n", errno);
      recorder->failed = 1;
      return 1;
    }

  recorder->first_index += recorder->count;
  recorder->count = 0;
  return 0;
}

/*
 * ADC A5 recorder のデバイス、出力バイナリ、RAMバッファを準備する。
 * 16kHz のサンプルを指定秒数分ためられる容量を優先し、足りなければ縮小する。
 */
int adc_a5_recorder_open(adc_a5_recorder_t *recorder, int capture_seconds)
{
  int target_samples;

  recorder->fd = -1;
  recorder->fp = NULL;
  recorder->buffer = NULL;
  recorder->capacity = 0;
  recorder->count = 0;
  recorder->first_index = 0;
  recorder->total = 0;
  recorder->failed = 0;

  /*
   * HPADC1 は poll() 未対応の環境があるため、非ブロッキング read() で
   * FIFO にたまった分だけ周期的に回収する。
   */
  recorder->fd = open(ADC_A5_DEVPATH, O_RDONLY | O_NONBLOCK);
  if (recorder->fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n", ADC_A5_DEVPATH, errno);
      recorder->failed = 1;
      return 1;
    }

  /* 収録中のSD書き込みを避けるため、まず収録全体分のRAM確保を試みる。 */
  target_samples = ADC_A5_RECORDER_SAMPLE_RATE_HZ * capture_seconds;
  for (recorder->capacity = target_samples;
       recorder->capacity >= 1;
       recorder->capacity /= 2)
    {
      recorder->buffer = (int16_t *)malloc(sizeof(int16_t) * recorder->capacity);
      if (recorder->buffer != NULL)
        {
          break;
        }
    }

  if (recorder->buffer == NULL)
    {
      printf("ERROR: ADC A5 output buffer allocation failed.\n");
      recorder->failed = 1;
      return 1;
    }

  recorder->fp = binary_file_open(ADC_A5_LOG_PATH);
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
      return 1;
    }

  if (adc_a5_recorder_write_header(recorder->fp))
    {
      recorder->failed = 1;
      return 1;
    }

  return 0;
}

/*
 * HPADC1/A5 のサンプリング条件を設定して開始する。
 * A5 は Spresense Arduino core 上で /dev/hpadc1 に対応する。
 */
int adc_a5_recorder_start(adc_a5_recorder_t *recorder)
{
  int ret;

  ret = ioctl(recorder->fd, SCUIOC_SETFIFOMODE, 1);
  if (ret < 0)
    {
      printf("ERROR: Failed to set ADC A5 FIFO overwrite mode. %d\n", errno);
      return 1;
    }

  ret = ioctl(recorder->fd, ANIOC_CXD56_FIFOSIZE, ADC_A5_FIFO_SIZE);
  if (ret < 0)
    {
      printf("ERROR: Failed to set ADC A5 FIFO size. %d\n", errno);
      return 1;
    }

  /*
   * 16kHz は SDK 設定値 CONFIG_CXD56_HPADC1_FREQ=7 を前提にする。
   * ioctl が未対応の環境でも既定設定で動けるよう、ここでは警告に留める。
   */
  ret = ioctl(recorder->fd, ANIOC_CXD56_FREQ, ADC_A5_FREQ_COEFFICIENT);
  if (ret < 0)
    {
      printf("WARNING: Failed to set ADC A5 sampling coefficient. %d\n", errno);
    }

#if defined(CONFIG_CXD56_HPADC1_FREQ)
  if (CONFIG_CXD56_HPADC1_FREQ != ADC_A5_FREQ_COEFFICIENT)
    {
      printf("WARNING: CONFIG_CXD56_HPADC1_FREQ is %d; expected %d for %d Hz.\n",
             CONFIG_CXD56_HPADC1_FREQ,
             ADC_A5_FREQ_COEFFICIENT,
             ADC_A5_RECORDER_SAMPLE_RATE_HZ);
    }
#endif

  ret = ioctl(recorder->fd, ANIOC_CXD56_START, 0);
  if (ret < 0)
    {
      printf("ERROR: Failed to start ADC A5 sampling. %d\n", errno);
      return 1;
    }

  printf("ADC A5 init: OK (%s, HPADC1, %d Hz)\n",
         ADC_A5_DEVPATH, ADC_A5_RECORDER_SAMPLE_RATE_HZ);
  return 0;
}

/* ADC A5 のサンプリングを停止する。停止失敗は警告に留めて後始末を続ける。 */
void adc_a5_recorder_stop(adc_a5_recorder_t *recorder)
{
  if (recorder->fd >= 0 && ioctl(recorder->fd, ANIOC_CXD56_STOP, 0) < 0)
    {
      printf("WARNING: Failed to stop ADC A5 sampling. %d\n", errno);
    }
}

/*
 * ADC A5 の保留サンプルをまとめて取り込む。
 * HPADC1 は poll() できない環境があるため、呼び出し元が非ブロッキング read() する。
 */
int adc_a5_recorder_read_ready(adc_a5_recorder_t *recorder)
{
  int i;
  int samples_read;
  int16_t readbuf[ADC_A5_READ_CHUNK_SAMPLES];
  ssize_t nbytes;

  if (recorder->count >= recorder->capacity && adc_a5_recorder_flush(recorder))
    {
      return -1;
    }

  nbytes = read(recorder->fd, readbuf, sizeof(readbuf));
  if (nbytes < 0)
    {
      if (adc_a5_is_read_empty(errno))
        {
          return 0;
        }

      printf("ERROR: ADC A5 read failed. %d\n", errno);
      recorder->failed = 1;
      return -1;
    }

  /* ドライバは int16_t の連続データを返すため、バイト数から件数へ変換する。 */
  samples_read = nbytes / sizeof(readbuf[0]);
  for (i = 0; i < samples_read; i++)
    {
      if (recorder->count >= recorder->capacity && adc_a5_recorder_flush(recorder))
        {
          return -1;
        }

      recorder->buffer[recorder->count] = readbuf[i];
      recorder->count++;
      recorder->total++;
    }

  return samples_read;
}

/*
 * 残った ADC サンプルを書き出し、バイナリ、デバイス、RAMバッファを解放する。
 * 戻り値は保存または後始末で失敗があったかどうかを示す。
 */
int adc_a5_recorder_finish(adc_a5_recorder_t *recorder)
{
  if (!recorder->failed && recorder->fp != NULL && recorder->count > 0)
    {
      adc_a5_recorder_flush(recorder);
    }

  if (recorder->fp != NULL && fclose(recorder->fp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", ADC_A5_LOG_PATH, errno);
      recorder->failed = 1;
    }

  if (recorder->fd >= 0)
    {
      close(recorder->fd);
    }

  free(recorder->buffer);

  if (!recorder->failed)
    {
      printf("Saved ADC A5 binary samples to %s.\n", ADC_A5_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", ADC_A5_LOG_PATH);
    }

  return recorder->failed;
}
