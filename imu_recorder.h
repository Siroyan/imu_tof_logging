#ifndef IMU_RECORDER_H
#define IMU_RECORDER_H

#include <nuttx/sensors/cxd5602pwbimu.h>

#include <stdio.h>

#define IMU_RECORDER_SAMPLE_RATE_HZ  1920

/* IMU デバイス、バイナリログ、RAMバッファの状態をまとめて保持する。 */
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

/* /dev/imu0、バイナリログ、収録バッファを準備する。 */
int imu_recorder_open(imu_recorder_t *recorder, int capture_seconds);

/* IMU のサンプリングを開始する。 */
int imu_recorder_start(imu_recorder_t *recorder);

/* IMU のサンプリングを停止する。 */
void imu_recorder_stop(imu_recorder_t *recorder);

/* 読み出し可能になった IMU サンプルをバッファへ取り込む。 */
int imu_recorder_read_ready(imu_recorder_t *recorder);

/* 残りサンプルの保存とリソース解放を行う。 */
int imu_recorder_finish(imu_recorder_t *recorder);

#endif /* IMU_RECORDER_H */
