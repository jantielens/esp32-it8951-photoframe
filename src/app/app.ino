#include "board_config.h"
#include "sd_photo_picker.h"
#include "it8951_renderer.h"

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
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Photoframe boot");

  SdCardPins pins = {
      .cs = SD_CS_PIN,
      .sck = SD_SCK_PIN,
      .miso = SD_MISO_PIN,
      .mosi = SD_MOSI_PIN,
      .power = SD_POWER_PIN,
  };

  if (!sd_photo_picker_init(sdSpi, pins, kSdFrequencyHz)) {
    Serial.println("SD init failed");
    enter_deep_sleep();
    return;
  }

  randomSeed(analogRead(0));

  char path[64];
  if (!sd_pick_random_bmp(path, sizeof(path))) {
    Serial.println("No BMP files found");
    enter_deep_sleep();
    return;
  }

  if (!it8951_renderer_init()) {
    Serial.println("Display init failed");
    enter_deep_sleep();
    return;
  }

  Serial.print("Rendering ");
  Serial.println(path);

  if (!it8951_render_bmp_from_sd(path)) {
    Serial.println("Render failed");
  }

  it8951_renderer_hibernate();
  enter_deep_sleep();
}

void loop() {}