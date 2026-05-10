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

#include <nuttx/sensors/cxd5602pwbimu.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CXD5602PWBIMU_DEVPATH      "/dev/imu0"
#define IMU_LOG_PATH               "/mnt/sd0/imu_log.csv"
#define MAX_CAPTURE_SECONDS        10
#define LOG_FLUSH_CHUNK_SAMPLES    128

#define itemsof(a) (sizeof(a)/sizeof(a[0]))

/****************************************************************************
 * Private values
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR FILE *open_log_file(FAR const char *path)
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
      if (fprintf(fp, "timestamp,temp,gx,gy,gz,ax,ay,az\n") < 0)
        {
          printf("ERROR: Failed to write CSV header. %d\n", errno);
          fclose(fp);
          return NULL;
        }
    }

  return fp;
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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * sensor_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int ret;
  int started = 0;
  int write_failed = 0;
  int buffered_samples = 0;
  int total_samples = 0;
  int buffer_samples;
  int flush_threshold;
  struct pollfd fds[1];
  struct timespec start, now, delta;
  cxd5602pwbimu_data_t *outbuf = NULL;
  FAR FILE *logfp = NULL;

  /* Sensing parameters, see start sensing function. */

  const int samplerate = 1920;
  const int adrange = 2;
  const int gdrange = 125;
  const int nfifos = 1;
  const int target_buffer_samples = samplerate * MAX_CAPTURE_SECONDS;

  fd = open(CXD5602PWBIMU_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      printf("ERROR: Device %s open failure. %d\n", CXD5602PWBIMU_DEVPATH, errno);
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
      return 1;
    }

  flush_threshold = LOG_FLUSH_CHUNK_SAMPLES;
  if (flush_threshold > buffer_samples)
    {
      flush_threshold = buffer_samples;
    }

  printf("Capture buffer: %d samples (%.3f sec)\n",
         buffer_samples, buffer_samples / (float)samplerate);

  fds[0].fd = fd;
  fds[0].events = POLLIN;

  logfp = open_log_file(IMU_LOG_PATH);
  if (logfp == NULL)
    {
      close(fd);
      free(outbuf);
      return 1;
    }

  ret = start_sensing(fd, samplerate, adrange, gdrange, nfifos);
  if (ret)
    {
      fclose(logfp);
      close(fd);
      free(outbuf);
      return ret;
    }

  memset(&now, 0, sizeof(now));
  memset(&start, 0, sizeof(start));
  memset(&delta, 0, sizeof(delta));

  while (1)
    {
      ret = poll(fds, 1, 1000);
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
                  started = 1;
                }

              buffered_samples++;
              total_samples++;
            }
          else
            {
              printf("ERROR: read size mismatch! %d\n", ret);
            }
        }

      if (buffered_samples >= flush_threshold)
        {
          ret = append_sensing_data(logfp, outbuf, outbuf + buffered_samples);
          if (ret)
            {
              write_failed = 1;
              break;
            }
          buffered_samples = 0;
        }

      if (started)
        {
          clock_gettime(CLOCK_MONOTONIC, &now);
          clock_timespec_subtract(&now, &start, &delta);
          if (delta.tv_sec >= MAX_CAPTURE_SECONDS)
            {
              break;
            }
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

  if (fclose(logfp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", IMU_LOG_PATH, errno);
      write_failed = 1;
    }

  close(fd);

  if (!write_failed)
    {
      printf("Saved samples to %s while sampling.\n", IMU_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", IMU_LOG_PATH);
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
  printf("Finished.\n");

  free(outbuf);

  return 0;
}
