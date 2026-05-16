#ifndef TOF_RECORDER_H
#define TOF_RECORDER_H

#include "binary_stream.h"

#include <stdint.h>
#include <stdio.h>

#define TOF_RECORDER_SAMPLE_PERIOD_MS 50
#define TOF_RECORDER_SAMPLE_RATE_HZ   (1000 / TOF_RECORDER_SAMPLE_PERIOD_MS)

typedef struct tof_sample_s
{
  uint64_t session_time_us;
  uint32_t sample_index;
  uint16_t distance_mm;
  uint8_t range_status;
  uint8_t timeout;
} tof_sample_t;

/* ToF センサ、バイナリログ、RAMバッファの状態をまとめて保持する。 */
typedef struct tof_recorder_s
{
  FILE *fp;
  binary_stream_t stream;
  int capacity;
  uint32_t total;
  uint64_t next_check_us;
  uint64_t session_start_us;
  int failed;
  int started;
} tof_recorder_t;

#ifdef __cplusplus
extern "C" {
#endif

/* VL53L1X、バイナリログ、収録バッファを準備する。 */
int tof_recorder_open(tof_recorder_t *recorder, int capture_seconds);

/* 共通収録開始時刻を含むToFバイナリヘッダを書き込む。 */
int tof_recorder_write_header(tof_recorder_t *recorder, uint64_t session_start_us);

/* I2C0上のVL53L1Xを初期化し、連続測距を開始する。 */
int tof_recorder_start(tof_recorder_t *recorder);

/* VL53L1Xの連続測距を停止する。 */
void tof_recorder_stop(tof_recorder_t *recorder);

/* 読み出し可能になったToF測距結果をバッファへ取り込む。 */
int tof_recorder_read_ready(tof_recorder_t *recorder);

/* 残りサンプルの保存とリソース解放を行う。 */
int tof_recorder_finish(tof_recorder_t *recorder);

#ifdef __cplusplus
}
#endif

#endif /* TOF_RECORDER_H */
