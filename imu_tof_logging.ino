#include <Arduino.h>
#include <arch/board/board.h>
#include <SDHCI.h>
#include <Wire.h>
#include <VL53L1X.h>

SDClass SD;

extern "C" int main(int argc, char *argv[]);

typedef struct tof_sample_s
{
  uint64_t timestamp_ms;
  int32_t distance_mm;
  int32_t status;
} tof_sample_t;

static VL53L1X g_tof4m;
static bool g_tof4m_ready = false;

static void init_tof4m(void)
{
  Wire.begin();
  Wire.setClock(400000);

  g_tof4m.setBus(&Wire);
  g_tof4m.setTimeout(20);
  if (!g_tof4m.init())
    {
      printf("WARNING: ToF4M init failed on I2C address 0x29.\n");
      return;
    }

  /* Use short mode + minimum timing budget for highest measurement rate. */
  g_tof4m.setDistanceMode(VL53L1X::Short);
  if (!g_tof4m.setMeasurementTimingBudget(20000))
    {
      printf("WARNING: ToF4M timing budget setup failed.\n");
    }
  g_tof4m.startContinuous(20);
  g_tof4m_ready = true;
  printf("ToF4M init: OK (I2C 0x29, continuous 20ms)\n");
}

extern "C" int tof4m_poll_sample(tof_sample_t *sample)
{
  if (sample == nullptr)
    {
      return -3;
    }

  if (!g_tof4m_ready)
    {
      sample->timestamp_ms = 0;
      sample->distance_mm = -1;
      sample->status = -1;
      return -1;
    }

  if (!g_tof4m.dataReady())
    {
      return 0;
    }

  sample->timestamp_ms = millis();
  sample->distance_mm = g_tof4m.read(false);
  sample->status = static_cast<int32_t>(g_tof4m.ranging_data.range_status);

  if (g_tof4m.timeoutOccurred())
    {
      return -2;
    }

  return 1;
}

void setup(void) {
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
      printf("WARNING: Failed to mount MicroSD card. IMU data save may fail.\n");
    }
  else
    {
      printf("SD.begin: OK\n");
    }

  init_tof4m();

  main(0, NULL);
}

void loop() {

}
