#include <Arduino.h>
#include <arch/board/board.h>
#include <SDHCI.h>
#include <VL53L1X.h>
#include <Wire.h>

SDClass SD;

extern "C" int main(int argc, char *argv[]);

#ifdef PIN_D02
#  define RECORD_TRIGGER_PIN     PIN_D02
#else
#  define RECORD_TRIGGER_PIN     2
#endif
#define RECORD_TRIGGER_NAME      "Digital2"
#define RECORD_TRIGGER_STABLE_MS 50

static int is_trigger_level_stable(int level)
{
  if (digitalRead(RECORD_TRIGGER_PIN) != level)
    {
      return 0;
    }

  delay(RECORD_TRIGGER_STABLE_MS);
  return digitalRead(RECORD_TRIGGER_PIN) == level;
}

/*
 * 拡張ボード Digital2 が LOW で待機してから HIGH になるまで待つ。
 * 拡張ボードのデジタル端子は未接続時に HIGH になり得るため、
 * 起動直後の HIGH はトリガとして扱わず、LOW確認後の立ち上がりだけを開始信号にする。
 */
static void wait_recording_trigger(void)
{
  pinMode(RECORD_TRIGGER_PIN, INPUT);
  printf("Waiting for %s LOW to arm recording trigger...\n",
         RECORD_TRIGGER_NAME);

  while (!is_trigger_level_stable(LOW))
    {
      delay(10);
    }

  printf("Recording trigger armed. Waiting for %s HIGH...\n",
         RECORD_TRIGGER_NAME);

  while (1)
    {
      if (is_trigger_level_stable(HIGH))
        {
          printf("Recording trigger detected.\n");
          return;
        }

      delay(10);
    }
}

void setup(void)
{
  int ret;
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

  wait_recording_trigger();
  main(0, NULL);
}

void loop()
{

}
