/****************************************************************************
 * examples/cxd5602pwbimu/cxd5602pwbimu_main.c
 *
 *   Copyright 2025 Sony Semiconductor Solutions Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of Sony Semiconductor Solutions Corporation nor
 *    the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <arch/cxd56xx/adc.h>
#include <arch/cxd56xx/scu.h>
#include <nuttx/sensors/cxd5602pwbimu.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CXD5602PWBIMU_DEVPATH      "/dev/imu0"
#define ADC_A5_DEVPATH             "/dev/hpadc1"
#define IMU_LOG_PATH               "/mnt/sd0/imu_log.csv"
#define ADC_A5_LOG_PATH            "/mnt/sd0/adc_a5_log.csv"
#define MAX_CAPTURE_SECONDS        10
#define ADC_A5_SAMPLE_RATE_HZ      16000
#define ADC_A5_FREQ_COEFFICIENT    7
#define ADC_READ_CHUNK_SAMPLES     128
#define ADC_A5_REFERENCE_VOLTAGE   5.0f

#ifdef CONFIG_CXD56_HPADC1_FSIZE
#  define ADC_A5_FIFO_SIZE         CONFIG_CXD56_HPADC1_FSIZE
#else
#  define ADC_A5_FIFO_SIZE         64
#endif

#ifdef CONFIG_CXD56_HPADC1_INPUT_GAIN_M6DB
#  define ADC_A5_RAW_MIN           (-29362)
#  define ADC_A5_RAW_MAX           (26375)
#else
#  define ADC_A5_RAW_MIN           SHRT_MIN
#  define ADC_A5_RAW_MAX           SHRT_MAX
#endif

#define itemsof(a) (sizeof(a)/sizeof(a[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR FILE *open_log_file(FAR const char *path,
                               FAR const char *header)
{
  struct stat st;
  int need_header = 0;
  FAR FILE *fp;

  if (stat(path, &st) != 0 || st.st_size == 0)
    {
      need_header = 1;
    }

  fp = fopen(path, "a");
  if (fp == NULL)
    {
      printf("ERROR: Failed to open log file %s. %d\n", path, errno);
      return NULL;
    }

  if (need_header)
    {
      if (fprintf(fp, "%s\n", header) < 0)
        {
          printf("ERROR: Failed to write CSV header. %d\n", errno);
          fclose(fp);
          return NULL;
        }
    }

  return fp;
}

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

static int append_sensing_data(FAR FILE *fp,
                               FAR cxd5602pwbimu_data_t *first,
                               FAR cxd5602pwbimu_data_t *last)
{
  FAR cxd5602pwbimu_data_t *p;

  for (p = first; p < last; p++)
    {
      if (fprintf(fp, "%.6f,%.6f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                  p->timestamp / 19200000.0f,
                  p->temp,
                  p->gx, p->gy, p->gz,
                  p->ax, p->ay, p->az) < 0)
        {
          printf("ERROR: Failed to write IMU data. %d\n", errno);
          return 1;
        }
    }

  if (fflush(fp) != 0)
    {
      printf("ERROR: Failed to flush IMU data to SD. %d\n", errno);
      return 1;
    }

  return 0;
}

static int append_adc_a5_data(FAR FILE *fp, uint32_t first_index,
                              FAR int16_t *first, FAR int16_t *last)
{
  FAR int16_t *p;
  uint32_t sample_index = first_index;

  for (p = first; p < last; p++)
    {
      if (fprintf(fp, "%" PRIu32 ",%.9f,%" PRId16 ",%.6f\n",
                  sample_index,
                  sample_index / (double)ADC_A5_SAMPLE_RATE_HZ,
                  *p,
                  adc_a5_raw_to_voltage(*p)) < 0)
        {
          printf("ERROR: Failed to write ADC A5 data. %d\n", errno);
          return 1;
        }

      sample_index++;
    }

  if (fflush(fp) != 0)
    {
      printf("ERROR: Failed to flush ADC A5 data to SD. %d\n", errno);
      return 1;
    }

  return 0;
}

static int start_sensing(int fd, int rate, int adrange, int gdrange,
                         int nfifos)
{
  cxd5602pwbimu_range_t range;
  int ret;

  /*
   * Set sampling rate. Available values (Hz) are below.
   *
   * 15 (default), 30, 60, 120, 240, 480, 960, 1920
   */

  ret = ioctl(fd, SNIOC_SSAMPRATE, rate);
  if (ret)
    {
      printf("ERROR: Set sampling rate failed. %d\n", errno);
      return 1;
    }

  /*
   * Set dynamic ranges for accelerometer and gyroscope.
   * Available values are below.
   *
   * accel: 2 (default), 4, 8, 16
   * gyro: 125 (default), 250, 500, 1000, 2000, 4000
   */

  range.accel = adrange;
  range.gyro = gdrange;
  ret = ioctl(fd, SNIOC_SDRANGE, (unsigned long)(uintptr_t)&range);
  if (ret)
    {
      printf("ERROR: Set dynamic range failed. %d\n", errno);
      return 1;
    }

  /*
   * Set hardware FIFO threshold.
   * Increasing this value will reduce the frequency with which data is
   * received.
   */

  ret = ioctl(fd, SNIOC_SFIFOTHRESH, nfifos);
  if (ret)
    {
      printf("ERROR: Set sampling rate failed. %d\n", errno);
      return 1;
    }

  /*
   * Start sensing, user can not change the all of configurations.
   */

  ret = ioctl(fd, SNIOC_ENABLE, 1);
  if (ret)
    {
      printf("ERROR: Enable failed. %d\n", errno);
      return 1;
    }

  return 0;
}

static int start_adc_a5(int fd)
{
  int ret;

  ret = ioctl(fd, SCUIOC_SETFIFOMODE, 1);
  if (ret < 0)
    {
      printf("ERROR: Failed to set ADC A5 FIFO overwrite mode. %d\n", errno);
      return 1;
    }

  ret = ioctl(fd, ANIOC_CXD56_FIFOSIZE, ADC_A5_FIFO_SIZE);
  if (ret < 0)
    {
      printf("ERROR: Failed to set ADC A5 FIFO size. %d\n", errno);
      return 1;
    }

  ret = ioctl(fd, ANIOC_CXD56_FREQ, ADC_A5_FREQ_COEFFICIENT);
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
             ADC_A5_SAMPLE_RATE_HZ);
    }
#endif

  ret = ioctl(fd, ANIOC_CXD56_START, 0);
  if (ret < 0)
    {
      printf("ERROR: Failed to start ADC A5 sampling. %d\n", errno);
      return 1;
    }

  printf("ADC A5 init: OK (%s, HPADC1, %d Hz)\n",
         ADC_A5_DEVPATH, ADC_A5_SAMPLE_RATE_HZ);

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * sensor_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int adcfd;
  int ret;
  int started = 0;
  int write_failed = 0;
  int adc_failed = 0;
  int buffered_samples = 0;
  int buffered_adc_samples = 0;
  int total_samples = 0;
  uint32_t total_adc_samples = 0;
  int buffer_samples;
  int adc_buffer_samples;
  int report_started = 0;
  int adc_samples_read;
  int i;
  ssize_t adc_nbytes;
  uint32_t adc_buffer_start_index = 0;
  int16_t adc_readbuf[ADC_READ_CHUNK_SAMPLES];
  struct pollfd fds[2];
  struct timespec start, now, delta, report_prev, report_delta;
  cxd5602pwbimu_data_t *outbuf = NULL;
  int16_t *adcoutbuf = NULL;
  FAR FILE *logfp = NULL;
  FAR FILE *adclogfp = NULL;

  /* Sensing parameters, see start sensing function. */

  const int samplerate = 1920;
  const int adrange = 2;
  const int gdrange = 125;
  const int nfifos = 1;
  const int target_buffer_samples = samplerate * MAX_CAPTURE_SECONDS;
  const int target_adc_buffer_samples =
    ADC_A5_SAMPLE_RATE_HZ * MAX_CAPTURE_SECONDS;

  fd = open(CXD5602PWBIMU_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n", CXD5602PWBIMU_DEVPATH, errno);
      return 1;
    }

  adcfd = open(ADC_A5_DEVPATH, O_RDONLY);
  if (adcfd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n", ADC_A5_DEVPATH, errno);
      close(fd);
      return 1;
    }

  buffer_samples = target_buffer_samples;
  while (buffer_samples >= 1)
    {
      outbuf = (cxd5602pwbimu_data_t *)malloc(sizeof(cxd5602pwbimu_data_t) *
                                              buffer_samples);
      if (outbuf != NULL)
        {
          break;
        }

      if (buffer_samples == 1)
        {
          break;
        }

      buffer_samples /= 2;
    }

  if (outbuf == NULL)
    {
      printf("ERROR: Output buffer allocation failed.\n");
      close(fd);
      close(adcfd);
      return 1;
    }

  adc_buffer_samples = target_adc_buffer_samples;
  while (adc_buffer_samples >= 1)
    {
      adcoutbuf = (int16_t *)malloc(sizeof(int16_t) * adc_buffer_samples);
      if (adcoutbuf != NULL)
        {
          break;
        }

      if (adc_buffer_samples == 1)
        {
          break;
        }

      adc_buffer_samples /= 2;
    }

  if (adcoutbuf == NULL)
    {
      printf("ERROR: ADC A5 output buffer allocation failed.\n");
      close(fd);
      close(adcfd);
      free(outbuf);
      return 1;
    }

  printf("Capture buffer: %d samples (%.3f sec)\n",
         buffer_samples, buffer_samples / (float)samplerate);
  printf("ADC A5 buffer: %d samples (%.3f sec)\n",
         adc_buffer_samples,
         adc_buffer_samples / (float)ADC_A5_SAMPLE_RATE_HZ);

  fds[0].fd = fd;
  fds[0].events = POLLIN;
  fds[1].fd = adcfd;
  fds[1].events = POLLIN;

  logfp = open_log_file(IMU_LOG_PATH, "timestamp,temp,gx,gy,gz,ax,ay,az");
  if (logfp == NULL)
    {
      close(fd);
      close(adcfd);
      free(outbuf);
      free(adcoutbuf);
      return 1;
    }

  adclogfp = open_log_file(ADC_A5_LOG_PATH,
                           "sample_index,timestamp_sec,raw,voltage_v");
  if (adclogfp == NULL)
    {
      fclose(logfp);
      close(fd);
      close(adcfd);
      free(outbuf);
      free(adcoutbuf);
      return 1;
    }

  ret = start_sensing(fd, samplerate, adrange, gdrange, nfifos);
  if (ret)
    {
      fclose(adclogfp);
      fclose(logfp);
      close(fd);
      close(adcfd);
      free(outbuf);
      free(adcoutbuf);
      return ret;
    }

  ret = start_adc_a5(adcfd);
  if (ret)
    {
      ioctl(fd, SNIOC_ENABLE, 0);
      fclose(adclogfp);
      fclose(logfp);
      close(fd);
      close(adcfd);
      free(outbuf);
      free(adcoutbuf);
      return ret;
    }

  memset(&now, 0, sizeof(now));
  memset(&start, 0, sizeof(start));
  memset(&delta, 0, sizeof(delta));
  memset(&report_prev, 0, sizeof(report_prev));
  memset(&report_delta, 0, sizeof(report_delta));

  while (1)
    {
      ret = poll(fds, 2, 1000);
      if (ret < 0)
        {
          if (errno != EINTR)
            {
              printf("ERROR: poll failed. %d\n", errno);
            }
          break;
        }
      if (ret == 0)
        {
          printf("Timeout!\n");
          continue;
        }

      if (fds[1].revents & POLLIN)
        {
          if (buffered_adc_samples >= adc_buffer_samples)
            {
              ret = append_adc_a5_data(adclogfp,
                                       adc_buffer_start_index,
                                       adcoutbuf,
                                       adcoutbuf + buffered_adc_samples);
              if (ret)
                {
                  adc_failed = 1;
                  break;
                }
              adc_buffer_start_index += buffered_adc_samples;
              buffered_adc_samples = 0;
            }

          adc_nbytes = read(adcfd, adc_readbuf, sizeof(adc_readbuf));
          if (adc_nbytes < 0)
            {
              printf("ERROR: ADC A5 read failed. %d\n", errno);
              adc_failed = 1;
              break;
            }

          adc_samples_read = adc_nbytes / sizeof(adc_readbuf[0]);
          for (i = 0; i < adc_samples_read; i++)
            {
              if (buffered_adc_samples >= adc_buffer_samples)
                {
                  ret = append_adc_a5_data(adclogfp,
                                           adc_buffer_start_index,
                                           adcoutbuf,
                                           adcoutbuf + buffered_adc_samples);
                  if (ret)
                    {
                      adc_failed = 1;
                      break;
                    }
                  adc_buffer_start_index += buffered_adc_samples;
                  buffered_adc_samples = 0;
                }

              adcoutbuf[buffered_adc_samples] = adc_readbuf[i];
              buffered_adc_samples++;
              total_adc_samples++;
            }

          if (adc_failed)
            {
              break;
            }
        }

      if (fds[0].revents & POLLIN)
        {
          if (buffered_samples >= buffer_samples)
            {
              ret = append_sensing_data(logfp, outbuf, outbuf + buffered_samples);
              if (ret)
                {
                  write_failed = 1;
                  break;
                }
              buffered_samples = 0;
            }

          ret = read(fd, &outbuf[buffered_samples], sizeof(*outbuf));
          if (ret == sizeof(*outbuf))
            {
              if (!started)
                {
                  /* To remove first sensing delay, start measurement from
                   * the first captured data.
                   */

                  clock_gettime(CLOCK_MONOTONIC, &start);
                  report_prev = start;
                  started = 1;
                  report_started = 1;
                }

              buffered_samples++;
              total_samples++;
            }
          else
            {
              printf("ERROR: read size mismatch! %d\n", ret);
            }
        }

      if (started)
        {
          clock_gettime(CLOCK_MONOTONIC, &now);
          clock_timespec_subtract(&now, &start, &delta);

          if (report_started)
            {
              clock_timespec_subtract(&now, &report_prev, &report_delta);
              if (report_delta.tv_sec > 0 ||
                  report_delta.tv_nsec >= 250000000L)
                {
                  printf("BUF %.1f%% (%d/%d)\n",
                         buffered_samples * 100.0f / (float)buffer_samples,
                         buffered_samples,
                         buffer_samples);
                  report_prev = now;
                }
            }

          if (delta.tv_sec >= MAX_CAPTURE_SECONDS)
            {
              break;
            }
        }
    }

  if (!adc_failed && buffered_adc_samples > 0)
    {
      ret = append_adc_a5_data(adclogfp,
                               adc_buffer_start_index,
                               adcoutbuf,
                               adcoutbuf + buffered_adc_samples);
      if (ret)
        {
          adc_failed = 1;
        }
    }

  if (!write_failed && buffered_samples > 0)
    {
      ret = append_sensing_data(logfp, outbuf, outbuf + buffered_samples);
      if (ret)
        {
          write_failed = 1;
        }
    }

  if (ioctl(adcfd, ANIOC_CXD56_STOP, 0) < 0)
    {
      printf("WARNING: Failed to stop ADC A5 sampling. %d\n", errno);
    }

  if (fclose(adclogfp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", ADC_A5_LOG_PATH, errno);
      adc_failed = 1;
    }

  if (fclose(logfp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", IMU_LOG_PATH, errno);
      write_failed = 1;
    }

  close(fd);
  close(adcfd);

  if (!write_failed)
    {
      printf("Saved samples to %s while sampling.\n", IMU_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", IMU_LOG_PATH);
    }

  if (!adc_failed)
    {
      printf("Saved ADC A5 samples to %s while sampling.\n", ADC_A5_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", ADC_A5_LOG_PATH);
    }

  if (started)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      clock_timespec_subtract(&now, &start, &delta);
    }
  else
    {
      memset(&delta, 0, sizeof(delta));
    }

  printf("Elapsed %ld.%09ld seconds\n", delta.tv_sec, delta.tv_nsec);
  printf("%d samples captured\n", total_samples);
  printf("%" PRIu32 " ADC A5 samples captured\n", total_adc_samples);
  printf("Finished.\n");

  free(outbuf);
  free(adcoutbuf);

  return (write_failed || adc_failed) ? 1 : 0;
}
