#include "board_config.h"
#include "sd_photo_picker.h"
#include "it8951_renderer.h"
#include "log_manager.h"

#include <SD.h>
#include <SPI.h>
#include <esp_sleep.h>

static constexpr uint32_t kSleepSeconds = 30;
static constexpr uint32_t kSdFrequencyHz = 80000000;

static SPIClass sdSpi(HSPI);

static bool build_g4_path(const char *bmp_path, char *g4_path, size_t g4_size) {
  if (!bmp_path || !g4_path || g4_size == 0) return false;
  const size_t len = strnlen(bmp_path, g4_size);
  if (len == 0 || len >= g4_size) return false;
  strncpy(g4_path, bmp_path, g4_size);
  g4_path[g4_size - 1] = '\0';
  char *dot = strrchr(g4_path, '.');
  if (!dot) {
    if (len + 3 >= g4_size) return false;
    strcat(g4_path, ".g4");
    return true;
  }
  if (strlen(dot) < 3) return false;
  strcpy(dot, ".g4");
  return true;
}

static void enter_deep_sleep() {
  Serial.flush();
  delay(200);
  esp_sleep_enable_timer_wakeup(kSleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  log_init(115200);
  delay(200);
  LOGI("PHASE1", "Boot");

  SdCardPins pins = {
      .cs = SD_CS_PIN,
      .sck = SD_SCK_PIN,
      .miso = SD_MISO_PIN,
      .mosi = SD_MOSI_PIN,
      .power = SD_POWER_PIN,
  };

  const unsigned long sd_start = millis();
  if (!sd_photo_picker_init(sdSpi, pins, kSdFrequencyHz)) {
    LOGE("SD", "Init failed");
    enter_deep_sleep();
    return;
  }
  LOG_DURATION("SD", "Init", sd_start);

  const char *bmp_path = "/L1007502.bmp";
  char g4_path[64];
  if (!build_g4_path(bmp_path, g4_path, sizeof(g4_path))) {
    LOGE("EINK", "G4 path build failed for %s", bmp_path);
    enter_deep_sleep();
    return;
  }

  const unsigned long disp_start = millis();
  if (!it8951_renderer_init()) {
    LOGE("EINK", "Init failed");
    enter_deep_sleep();
    return;
  }
  LOG_DURATION("EINK", "Init", disp_start);

  LOGI("EINK", "Render G4=%s", g4_path);
  if (!it8951_render_g4(g4_path)) {
    LOGE("EINK", "Render G4 failed");
  }

  it8951_renderer_hibernate();
  LOGI("PHASE1", "Sleep %lus", (unsigned long)kSleepSeconds);
  enter_deep_sleep();
}

void loop() {}