#ifndef ADC_A5_RECORDER_H
#define ADC_A5_RECORDER_H

#include "binary_stream.h"

#include <stdint.h>
#include <stdio.h>

#define ADC_A5_RECORDER_SAMPLE_RATE_HZ 16000

/* ADC A5 デバイス、バイナリログ、RAMバッファの状態をまとめて保持する。 */
typedef struct adc_a5_recorder_s
{
  int fd;
  FILE *fp;
  binary_stream_t stream;
  int capacity;
  uint32_t total;
  uint64_t session_start_us;
  int failed;
} adc_a5_recorder_t;

/* /dev/hpadc1、バイナリログ、収録バッファを準備する。 */
int adc_a5_recorder_open(adc_a5_recorder_t *recorder, int capture_seconds);

/* 共通収録開始時刻を含むADC A5バイナリヘッダを書き込む。 */
int adc_a5_recorder_write_header(adc_a5_recorder_t *recorder,
                                 uint64_t session_start_us);

/* HPADC1/A5 のサンプリングを開始する。 */
int adc_a5_recorder_start(adc_a5_recorder_t *recorder);

/* HPADC1/A5 のサンプリングを停止する。 */
void adc_a5_recorder_stop(adc_a5_recorder_t *recorder);

/* 読み出し可能になった ADC サンプルをバッファへ取り込む。 */
int adc_a5_recorder_read_ready(adc_a5_recorder_t *recorder);

/* 残りサンプルの保存とリソース解放を行う。 */
int adc_a5_recorder_finish(adc_a5_recorder_t *recorder);

#endif /* ADC_A5_RECORDER_H */
