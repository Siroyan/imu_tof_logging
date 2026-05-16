#include "adc_a5_recorder.h"

#include "csv_file.h"

#include <nuttx/config.h>
#include <arch/cxd56xx/adc.h>
#include <arch/cxd56xx/scu.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ADC_A5_DEVPATH           "/dev/hpadc1"
#define ADC_A5_LOG_PATH          "/mnt/sd0/adc_a5_log.csv"
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

static float adc_a5_raw_to_voltage(int16_t raw)
{
  int32_t clipped = raw;

  if (clipped < ADC_A5_RAW_MIN)
    {
      clipped = ADC_A5_RAW_MIN;
    }
  else if (clipped > ADC_A5_RAW_MAX)
    {
      clipped = ADC_A5_RAW_MAX;
    }

  return (clipped - ADC_A5_RAW_MIN) * ADC_A5_REFERENCE_VOLTAGE /
         (float)(ADC_A5_RAW_MAX - ADC_A5_RAW_MIN);
}

static int adc_a5_recorder_flush(adc_a5_recorder_t *recorder)
{
  int16_t *p;
  uint32_t sample_index = recorder->first_index;

  if (recorder->count == 0)
    {
      return 0;
    }

  for (p = recorder->buffer; p < recorder->buffer + recorder->count; p++)
    {
      if (fprintf(recorder->fp, "%" PRIu32 ",%.9f,%" PRId16 ",%.6f\n",
                  sample_index,
                  sample_index / (double)ADC_A5_RECORDER_SAMPLE_RATE_HZ,
                  *p,
                  adc_a5_raw_to_voltage(*p)) < 0)
        {
          printf("ERROR: Failed to write ADC A5 data. %d\n", errno);
          recorder->failed = 1;
          return 1;
        }

      sample_index++;
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

  recorder->fd = open(ADC_A5_DEVPATH, O_RDONLY);
  if (recorder->fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n", ADC_A5_DEVPATH, errno);
      recorder->failed = 1;
      return 1;
    }

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

  recorder->fp = csv_file_open(ADC_A5_LOG_PATH,
                               "sample_index,timestamp_sec,raw,voltage_v");
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
    }

  return recorder->fp == NULL ? 1 : 0;
}

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

void adc_a5_recorder_stop(adc_a5_recorder_t *recorder)
{
  if (recorder->fd >= 0 && ioctl(recorder->fd, ANIOC_CXD56_STOP, 0) < 0)
    {
      printf("WARNING: Failed to stop ADC A5 sampling. %d\n", errno);
    }
}

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
      printf("ERROR: ADC A5 read failed. %d\n", errno);
      recorder->failed = 1;
      return -1;
    }

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
      printf("Saved ADC A5 samples to %s while sampling.\n", ADC_A5_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", ADC_A5_LOG_PATH);
    }

  return recorder->failed;
}
