#include <Arduino.h>
#include <arch/board/board.h>
#include <SDHCI.h>
#include <VL53L1X.h>
#include <Wire.h>

SDClass SD;

extern "C" int main(int argc, char *argv[]);

#define RECORDING_AUTO_START_DELAY_MS 3000
#define STATUS_LED_BLINK_MS      500

static const uint8_t STATUS_LEDS[] = {LED0, LED1, LED2, LED3};

static void set_status_leds(int led0, int led1, int led2, int led3)
{
  digitalWrite(STATUS_LEDS[0], led0 ? HIGH : LOW);
  digitalWrite(STATUS_LEDS[1], led1 ? HIGH : LOW);
  digitalWrite(STATUS_LEDS[2], led2 ? HIGH : LOW);
  digitalWrite(STATUS_LEDS[3], led3 ? HIGH : LOW);
}

static void initialize_status_leds(void)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      pinMode(STATUS_LEDS[i], OUTPUT);
    }

  set_status_leds(0, 0, 0, 0);
}

/* レコーディング待機中は LED0 を点滅させる。 */
static void update_waiting_leds(void)
{
  static int initialized = 0;
  static int led_on = 0;
  static unsigned long last_toggle_ms = 0;
  unsigned long now_ms = millis();

  if (!initialized || now_ms - last_toggle_ms >= STATUS_LED_BLINK_MS)
    {
      initialized = 1;
      led_on = !led_on;
      last_toggle_ms = now_ms;
      set_status_leds(led_on, 0, 0, 0);
    }
}

/* レコーディング中は4灯を点灯させる。 */
static void set_recording_leds(void)
{
  set_status_leds(1, 1, 1, 1);
}

/*
 * レコーディング後は LED3 を点灯させる。
 * エラー終了時は LED0 も点灯させ、正常終了と区別する。
 */
static void set_finished_leds(int result)
{
  if (result == 0)
    {
      set_status_leds(0, 0, 0, 1);
    }
  else
    {
      set_status_leds(1, 0, 0, 1);
    }
}

/* 起動後の固定待機時間が終わるまで、待機中LEDを更新しながら待つ。 */
static void wait_recording_start(unsigned long start_ms)
{
  printf("Recording starts automatically after %lu ms from boot.\n",
         (unsigned long)RECORDING_AUTO_START_DELAY_MS);

  while ((long)(millis() - start_ms) < 0)
    {
      update_waiting_leds();
      delay(10);
    }

  printf("Auto recording start.\n");
}

void setup(void)
{
  int ret;
  int main_result;
  unsigned long recording_start_ms;

  initialize_status_leds();
  update_waiting_leds();
  recording_start_ms = millis() + RECORDING_AUTO_START_DELAY_MS;

  ret = board_cxd5602pwbimu_initialize(5);
  if (ret < 0)
    {
      printf("ERROR: Failed to initialize CXD5602PWBIMU.\n");
    }
  else
    {
      printf("board_cxd5602pwbimu_initialize: OK\n");
    }

  if (!SD.begin())
    {
      printf("WARNING: Failed to mount MicroSD card. Data save may fail.\n");
    }
  else
    {
      printf("SD.begin: OK\n");
    }

  wait_recording_start(recording_start_ms);
  set_recording_leds();
  main_result = main(0, NULL);
  set_finished_leds(main_result);
}

void loop()
{

}
