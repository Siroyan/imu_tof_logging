#ifndef ADC_A5_RECORDER_H
#define ADC_A5_RECORDER_H

#include <stdint.h>
#include <stdio.h>

#define ADC_A5_RECORDER_SAMPLE_RATE_HZ 16000

typedef struct adc_a5_recorder_s
{
  int fd;
  FILE *fp;
  int16_t *buffer;
  int capacity;
  int count;
  uint32_t first_index;
  uint32_t total;
  int failed;
} adc_a5_recorder_t;

int adc_a5_recorder_open(adc_a5_recorder_t *recorder, int capture_seconds);
int adc_a5_recorder_start(adc_a5_recorder_t *recorder);
void adc_a5_recorder_stop(adc_a5_recorder_t *recorder);
int adc_a5_recorder_read_ready(adc_a5_recorder_t *recorder);
int adc_a5_recorder_finish(adc_a5_recorder_t *recorder);

#endif /* ADC_A5_RECORDER_H */
