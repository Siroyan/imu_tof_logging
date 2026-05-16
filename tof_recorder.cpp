#include "tof_recorder.h"

#include "binary_file.h"

#include <Arduino.h>
#include <VL53L1X.h>
#include <Wire.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TOF_LOG_PATH             "/mnt/sd0/tof_log.bin"
#define TOF_LOG_MAGIC            "TOFLOG1"
#define TOF_LOG_VERSION          2
#define TOF_I2C_ADDRESS          0x29
#define TOF_I2C_CLOCK_HZ         400000
#define TOF_WIRE_TIMEOUT_US      100000
#define TOF_SENSOR_TIMEOUT_MS    100
#define TOF_TIMING_BUDGET_US     50000
#define TOF_CHECK_INTERVAL_US    5000
#define TOF_STREAM_BUFFER_SAMPLES 64

typedef struct tof_log_header_s
{
  char magic[8];
  uint32_t version;
  uint32_t i2c_address;
  uint32_t sample_period_ms;
  uint32_t timing_budget_us;
  uint32_t record_size;
  uint32_t reserved;
  uint64_t session_start_us;
} tof_log_header_t;

static VL53L1X g_tof_sensor;

/* CLOCK_MONOTONIC を us 単位に変換する。 */
static uint64_t tof_recorder_monotonic_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* 共通収録開始時刻を原点にしたToF読み出し時刻を作る。 */
static uint64_t tof_recorder_session_time_us(const tof_recorder_t *recorder,
                                             uint64_t now_us)
{
  if (now_us < recorder->session_start_us)
    {
      return 0;
    }

  return now_us - recorder->session_start_us;
}

/*
 * ToF バイナリログの先頭ヘッダを書き込む。
 * サンプル本体は、共通開始時刻からの経過usを持つ tof_sample_t として保存する。
 */
int tof_recorder_write_header(tof_recorder_t *recorder, uint64_t session_start_us)
{
  tof_log_header_t header = {
    TOF_LOG_MAGIC,
    TOF_LOG_VERSION,
    TOF_I2C_ADDRESS,
    TOF_RECORDER_SAMPLE_PERIOD_MS,
    TOF_TIMING_BUDGET_US,
    sizeof(tof_sample_t),
    0,
    session_start_us
  };

  recorder->session_start_us = session_start_us;
  return binary_file_write(recorder->fp, &header, sizeof(header), 1,
                           "ToF binary header");
}

/*
 * ToF recorder の出力バイナリとRAMバッファを準備する。
 * VL53L1X は低レートなので、小さな2面バッファでSD書き込みを分離する。
 */
int tof_recorder_open(tof_recorder_t *recorder, int capture_seconds)
{
  (void)capture_seconds;

  recorder->fp = NULL;
  memset(&recorder->stream, 0, sizeof(recorder->stream));
  recorder->capacity = TOF_STREAM_BUFFER_SAMPLES;
  recorder->total = 0;
  recorder->next_check_us = 0;
  recorder->session_start_us = 0;
  recorder->failed = 0;
  recorder->started = 0;

  recorder->fp = binary_file_open(TOF_LOG_PATH);
  if (recorder->fp == NULL)
    {
      recorder->failed = 1;
      return 1;
    }

  if (binary_stream_open(&recorder->stream,
                         recorder->fp,
                         sizeof(tof_sample_t),
                         recorder->capacity,
                         "ToF samples"))
    {
      recorder->failed = 1;
      return 1;
    }

  return 0;
}

/*
 * I2C0(Wire) のVL53L1Xを初期化し、連続測距を開始する。
 * 測距周期はタイミングバジェットと同じ50msにして、約20Hzで記録する。
 */
int tof_recorder_start(tof_recorder_t *recorder)
{
  Wire.begin();
  Wire.setClock(TOF_I2C_CLOCK_HZ);
  Wire.setWireTimeout(TOF_WIRE_TIMEOUT_US, true);

  g_tof_sensor.setBus(&Wire);
  g_tof_sensor.setTimeout(TOF_SENSOR_TIMEOUT_MS);
  if (!g_tof_sensor.init())
    {
      printf("ERROR: Failed to initialize VL53L1X at I2C address 0x%02x.\n",
             TOF_I2C_ADDRESS);
      recorder->failed = 1;
      return 1;
    }

  if (!g_tof_sensor.setDistanceMode(VL53L1X::Long))
    {
      printf("ERROR: Failed to set VL53L1X distance mode.\n");
      recorder->failed = 1;
      return 1;
    }

  if (!g_tof_sensor.setMeasurementTimingBudget(TOF_TIMING_BUDGET_US))
    {
      printf("ERROR: Failed to set VL53L1X timing budget.\n");
      recorder->failed = 1;
      return 1;
    }

  g_tof_sensor.startContinuous(TOF_RECORDER_SAMPLE_PERIOD_MS);
  recorder->started = 1;
  recorder->next_check_us = 0;

  printf("ToF init: OK (VL53L1X, I2C0, 0x%02x, %d ms)\n",
         TOF_I2C_ADDRESS, TOF_RECORDER_SAMPLE_PERIOD_MS);
  return 0;
}

/* VL53L1X の連続測距を停止する。停止後も保存処理は続ける。 */
void tof_recorder_stop(tof_recorder_t *recorder)
{
  if (recorder->started)
    {
      g_tof_sensor.stopContinuous();
      recorder->started = 0;
    }
}

/*
 * 新しい測距結果があれば1件取り込む。
 * IMUループから高頻度に呼ばれるため、I2C確認は5ms間隔に間引く。
 */
int tof_recorder_read_ready(tof_recorder_t *recorder)
{
  tof_sample_t *sample;
  tof_sample_t sample_data;
  uint64_t now_us;
  uint16_t distance_mm;

  if (!recorder->started)
    {
      return 0;
    }

  now_us = tof_recorder_monotonic_us();
  if (now_us < recorder->next_check_us)
    {
      return 0;
    }

  recorder->next_check_us = now_us + TOF_CHECK_INTERVAL_US;
  if (!g_tof_sensor.dataReady())
    {
      return 0;
    }

  distance_mm = g_tof_sensor.read(false);
  sample = &sample_data;
  sample->session_time_us = tof_recorder_session_time_us(recorder, now_us);
  sample->sample_index = recorder->total;
  sample->distance_mm = distance_mm;
  sample->range_status = (uint8_t)g_tof_sensor.ranging_data.range_status;
  sample->timeout = g_tof_sensor.timeoutOccurred() ? 1 : 0;

  if (binary_stream_append(&recorder->stream, sample))
    {
      recorder->failed = 1;
      return -1;
    }

  recorder->total++;
  return 1;
}

/*
 * 残った ToF サンプルを書き出し、バイナリとRAMバッファを解放する。
 * 戻り値は保存または後始末で失敗があったかどうかを示す。
 */
int tof_recorder_finish(tof_recorder_t *recorder)
{
  if (recorder->started)
    {
      tof_recorder_stop(recorder);
    }

  if (recorder->fp != NULL && binary_stream_close(&recorder->stream))
    {
      recorder->failed = 1;
    }

  if (recorder->fp != NULL && fclose(recorder->fp) != 0)
    {
      printf("ERROR: Failed to close log file %s. %d\n", TOF_LOG_PATH, errno);
      recorder->failed = 1;
    }

  if (!recorder->failed)
    {
      printf("Saved ToF binary samples to %s.\n", TOF_LOG_PATH);
    }
  else
    {
      printf("WARNING: Failed to save all samples to %s\n", TOF_LOG_PATH);
    }

  return recorder->failed;
}
