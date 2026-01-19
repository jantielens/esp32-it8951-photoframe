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

  randomSeed(analogRead(0));

  char path[64];
  const unsigned long pick_start = millis();
  if (!sd_pick_random_bmp(path, sizeof(path))) {
    LOGW("SD", "No BMP files found");
    enter_deep_sleep();
    return;
  }
  LOG_DURATION("SD", "Pick", pick_start);

  const unsigned long disp_start = millis();
  if (!it8951_renderer_init()) {
    LOGE("EINK", "Init failed");
    enter_deep_sleep();
    return;
  }
  LOG_DURATION("EINK", "Init", disp_start);

  LOGI("EINK", "Render path=%s", path);

  const unsigned long render_start = millis();
  if (!it8951_render_bmp_from_sd(path)) {
    LOGE("EINK", "Render failed");
  } else {
    LOG_DURATION("EINK", "Render", render_start);
  }

  it8951_renderer_hibernate();
  LOGI("PHASE1", "Sleep %lus", (unsigned long)kSleepSeconds);
  enter_deep_sleep();
}

void loop() {}