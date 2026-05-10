#include <Arduino.h>
#include <arch/board/board.h>
#include <SDHCI.h>

SDClass SD;

extern "C" int main(int argc, char *argv[]);

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

  main(0, NULL);
}

void loop() {

}
