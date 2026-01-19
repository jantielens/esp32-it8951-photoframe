#include "board_config.h"
#include "sd_photo_picker.h"
#include "it8951_renderer.h"
#include "log_manager.h"

#include <SD.h>
#include <SPI.h>
#include <esp_sleep.h>

static constexpr uint32_t kSleepSeconds = 20;
static constexpr uint32_t kSdFrequencyHz = 80000000;

static SPIClass sdSpi(HSPI);

static bool is_g4_file(const char *name) {
  if (!name) return false;
  const char *suffix = ".g4";
  const size_t name_len = strlen(name);
  const size_t suffix_len = strlen(suffix);
  if (name_len < suffix_len) return false;
  return strcmp(name + (name_len - suffix_len), suffix) == 0;
}

static bool pick_random_g4(char *path, size_t path_size) {
  if (!path || path_size == 0) return false;

  File root = SD.open("/");
  if (!root) return false;

  uint32_t count = 0;
  bool found = false;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      const char *name = entry.name();
      if (is_g4_file(name)) {
        count++;
        if (random(count) == 0) {
          size_t len = strlen(name);
          if (len + 1 < path_size) {
            path[0] = '/';
            memcpy(path + 1, name, len + 1);
            found = true;
          }
        }
      }
    }
    entry.close();
  }

  root.close();
  return found;
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

  randomSeed(analogRead(0));

  char g4_path[64];
  if (!pick_random_g4(g4_path, sizeof(g4_path))) {
    LOGW("SD", "No .g4 files found");
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