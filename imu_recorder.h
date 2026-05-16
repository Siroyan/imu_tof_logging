#ifndef IMU_RECORDER_H
#define IMU_RECORDER_H

#include <nuttx/sensors/cxd5602pwbimu.h>

#include <stdio.h>

#define IMU_RECORDER_SAMPLE_RATE_HZ  1920

typedef struct imu_recorder_s
{
  int fd;
  FILE *fp;
  cxd5602pwbimu_data_t *buffer;
  int capacity;
  int count;
  int total;
  int failed;
} imu_recorder_t;

int imu_recorder_open(imu_recorder_t *recorder, int capture_seconds);
int imu_recorder_start(imu_recorder_t *recorder);
void imu_recorder_stop(imu_recorder_t *recorder);
int imu_recorder_read_ready(imu_recorder_t *recorder);
int imu_recorder_finish(imu_recorder_t *recorder);

#endif /* IMU_RECORDER_H */
