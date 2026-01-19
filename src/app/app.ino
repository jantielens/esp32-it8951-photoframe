#include "board_config.h"
#include "sd_photo_picker.h"
#include "it8951_renderer.h"
#include "log_manager.h"
#include "config_manager.h"
#include "rtc_state.h"
#include "web_portal.h"
#include "web_portal_sd_images.h"
#include "device_telemetry.h"
#if HEALTH_HISTORY_ENABLED
#include "health_history.h"
#endif

#include <SD.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <WiFi.h>
#include <ESPmDNS.h>

static constexpr uint32_t kDefaultSleepSeconds = 60;
static constexpr uint32_t kSdFrequencyHz = 80000000;
static constexpr uint16_t kDefaultLongPressMs = 1500;
static constexpr uint8_t kButtonActiveLevel = LOW;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint8_t kTouchGpio = 6; // TOUCH06 -> GPIO6 on ESP32-S2
static constexpr uint8_t kTouchSamples = 8;
static constexpr float kTouchThresholdRatio = 1.3f;
static constexpr uint32_t kTouchDebounceMs = 250;

static SPIClass sdSpi(HSPI);
static bool sd_ready = false;
static bool touch_ready = false;
static uint32_t touch_baseline = 0;
static uint32_t touch_threshold = 0;
static bool touch_active = false;
static unsigned long touch_last_trigger_ms = 0;

static void enter_deep_sleep(uint32_t sleep_seconds) {
  Serial.flush();
  delay(200);
  esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);
  if (BUTTON_PIN >= 0) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(BUTTON_PIN));
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(BUTTON_PIN));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(BUTTON_PIN));
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BUTTON_PIN), kButtonActiveLevel);
  }
  if (touch_ready) {
    esp_sleep_enable_touchpad_wakeup();
    LOGI("Touch", "Deep sleep touch wake enabled (baseline=%lu threshold=%lu)",
         (unsigned long)touch_baseline, (unsigned long)touch_threshold);
  }
  esp_deep_sleep_start();
}

static void init_button_pin() {
  if (BUTTON_PIN >= 0) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
  }
}

static bool is_button_pressed() {
  if (BUTTON_PIN < 0) return false;
  return digitalRead(BUTTON_PIN) == kButtonActiveLevel;
}

static bool check_long_press(uint16_t long_press_ms) {
  if (!is_button_pressed()) return false;
  const unsigned long start_ms = millis();
  while (millis() - start_ms < long_press_ms) {
    if (!is_button_pressed()) return false;
    delay(10);
  }
  return true;
}

static bool touch_read_value(uint32_t *out_value) {
  if (!out_value) return false;
  *out_value = (uint32_t)touchRead(kTouchGpio);
  return true;
}

static bool init_touch_pad() {
  touch_ready = false;
  touch_baseline = 0;
  touch_threshold = 0;

  uint32_t sum = 0;
  uint8_t samples = 0;
  bool skipped_first = false;
  for (uint8_t i = 0; i < kTouchSamples; i++) {
    uint32_t value = 0;
    if (touch_read_value(&value)) {
      if (!skipped_first) {
        skipped_first = true;
      } else {
        sum += value;
        samples++;
      }
    }
    delay(20);
  }

  if (samples == 0) {
    LOGW("Touch", "Calibration failed (no samples)");
    return false;
  }

  touch_baseline = sum / samples;
  touch_threshold = (uint32_t)((float)touch_baseline * kTouchThresholdRatio);

  touch_ready = true;
  LOGI("Touch", "Calibrated baseline=%lu threshold=%lu (ratio=%.2f) gpio=%u",
       (unsigned long)touch_baseline, (unsigned long)touch_threshold, kTouchThresholdRatio, kTouchGpio);
  return true;
}

static bool poll_touch_trigger() {
  if (!touch_ready) return false;

  uint32_t value = 0;
  if (!touch_read_value(&value)) {
    return false;
  }

  const unsigned long now = millis();

  const bool touched = (value > touch_threshold);
  if (touched && !touch_active && (now - touch_last_trigger_ms) > kTouchDebounceMs) {
    touch_active = true;
    touch_last_trigger_ms = now;
    LOGI("Touch", "Trigger value=%lu threshold=%lu", (unsigned long)value, (unsigned long)touch_threshold);
    return true;
  }

  if (!touched && touch_active) {
    touch_active = false;
  }

  return false;
}

static bool poll_button_click() {
  if (BUTTON_PIN < 0) return false;
  static bool last_read = false;
  static bool stable_state = false;
  static unsigned long last_change_ms = 0;

  const unsigned long now = millis();
  const bool pressed = is_button_pressed();

  if (pressed != last_read) {
    last_read = pressed;
    last_change_ms = now;
  }

  if ((now - last_change_ms) >= kButtonDebounceMs && stable_state != last_read) {
    stable_state = last_read;
    if (stable_state) {
      return true;
    }
  }

  return false;
}

static bool connect_wifi_simple(const DeviceConfig &config) {
  if (strlen(config.wifi_ssid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifi_ssid, config.wifi_password);

  for (int attempt = 0; attempt < WIFI_MAX_ATTEMPTS; attempt++) {
    const unsigned long start = millis();
    while (millis() - start < 3000) {
      if (WiFi.status() == WL_CONNECTED) {
        LOGI("WiFi", "Connected: %s", WiFi.localIP().toString().c_str());
        return true;
      }
      delay(100);
    }
    LOGW("WiFi", "Connect attempt %d/%d failed", attempt + 1, WIFI_MAX_ATTEMPTS);
  }

  return false;
}

static void start_mdns_simple(const DeviceConfig &config) {
  char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
  config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
  if (strlen(sanitized) == 0) return;

  if (MDNS.begin(sanitized)) {
    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS", "Name: %s.local", sanitized);
  } else {
    LOGW("mDNS", "Start failed");
  }
}

static bool init_sd_for_portal() {
  SdCardPins pins = {
      .cs = SD_CS_PIN,
      .sck = SD_SCK_PIN,
      .miso = SD_MISO_PIN,
      .mosi = SD_MOSI_PIN,
      .power = SD_POWER_PIN,
  };
  if (!sd_photo_picker_init(sdSpi, pins, kSdFrequencyHz)) {
    delay(200);
    if (!sd_photo_picker_init(sdSpi, pins, kSdFrequencyHz)) {
      return false;
    }
  }
  return true;
}

static bool ensure_sd_ready() {
  if (sd_ready) return true;
  if (!init_sd_for_portal()) {
    LOGW("SD", "Init failed (portal mode) - SD APIs unavailable");
    return false;
  }
  sd_ready = true;
  return true;
}

static void start_portal(DeviceConfig &config, bool config_loaded) {
  LOGI("Portal", "Portal start");

  if (!ensure_sd_ready()) {
    // SD APIs will be unavailable; keep portal running anyway.
  }

  if (!config_loaded || strlen(config.wifi_ssid) == 0) {
    LOGI("WiFi", "No config - starting AP mode");
    web_portal_start_ap();
  } else if (connect_wifi_simple(config)) {
    start_mdns_simple(config);
  } else {
    LOGW("WiFi", "Connect failed - fallback to AP mode");
    web_portal_start_ap();
  }

  web_portal_init(&config);
}

static bool render_next_image(DeviceConfig &config) {
  if (!sd_images_try_lock("auto_render")) {
    return false;
  }

  if (!ensure_sd_ready()) {
    sd_images_unlock();
    return false;
  }

  char g4_path[64];
  const SdImageSelectMode mode = (strcmp(config.image_selection_mode, "sequential") == 0)
      ? SdImageSelectMode::Sequential
      : SdImageSelectMode::Random;
  uint32_t working_index = rtc_image_state_get_last_image_index();
  char working_name_buf[64] = {0};
  const char *rtc_name = rtc_image_state_get_last_image_name();
  if (rtc_name && rtc_name[0] != '\0') {
    strlcpy(working_name_buf, rtc_name, sizeof(working_name_buf));
  }

  const uint8_t max_attempts = (mode == SdImageSelectMode::Sequential) ? 3 : 1;
  for (uint8_t attempt = 0; attempt < max_attempts; attempt++) {
    uint32_t selected_index = 0;
    char selected_name[64] = {0};
    if (!sd_pick_g4_image(g4_path, sizeof(g4_path), mode, working_index, working_name_buf, &selected_index, selected_name, sizeof(selected_name))) {
      LOGW("SD", "No .g4 files found");
      sd_images_unlock();
      return false;
    }

    if (!it8951_renderer_init()) {
      LOGE("EINK", "Init failed");
      sd_images_unlock();
      return false;
    }

    LOGI("EINK", "Render G4=%s", g4_path);
    if (!it8951_render_g4(g4_path)) {
      LOGE("EINK", "Render G4 failed");
      working_index = selected_index;
      strlcpy(working_name_buf, selected_name, sizeof(working_name_buf));
      continue;
    }

    if (mode == SdImageSelectMode::Sequential) {
      rtc_image_state_set_last_image_index(selected_index);
      rtc_image_state_set_last_image_name(selected_name);
    }

    sd_images_unlock();
    return true;
  }

  sd_images_unlock();
  return false;
}

static void run_config_mode(DeviceConfig &config, bool config_loaded) {
  LOGI("Portal", "Config mode start");

  start_portal(config, config_loaded);

  while (true) {
    web_portal_handle();
    sd_images_process_pending_display();
    delay(10);
  }
}

static void run_always_on(DeviceConfig &config, bool config_loaded) {
  LOGI("Mode", "Always-on enabled");

  start_portal(config, config_loaded);

  const uint32_t sleep_seconds = config.sleep_timeout_seconds > 0
      ? config.sleep_timeout_seconds
      : kDefaultSleepSeconds;
  const uint32_t refresh_interval_ms = sleep_seconds * 1000UL;

  unsigned long last_refresh_ms = 0;
  bool pending_refresh = true;

  while (true) {
    web_portal_handle();

    if (sd_images_process_pending_display()) {
      last_refresh_ms = millis();
      pending_refresh = false;
    }

    if (poll_button_click()) {
      pending_refresh = true;
    }

    if (poll_touch_trigger()) {
      pending_refresh = true;
    }

    const unsigned long now = millis();
    if (pending_refresh || (refresh_interval_ms > 0 && (now - last_refresh_ms >= refresh_interval_ms))) {
      if (render_next_image(config)) {
        last_refresh_ms = now;
        pending_refresh = false;
      }
    }

    delay(10);
  }
}

void setup() {
  #if HEALTH_HISTORY_ENABLED
  health_history_start();
  #endif

  init_button_pin();
  log_init(115200);
  delay(200);
  LOGI("Boot", "Boot");

  init_touch_pad();

  device_telemetry_init();
  device_telemetry_start_cpu_monitoring();
  device_telemetry_start_health_window_sampling();

  DeviceConfig config = {};
  config_manager_init();
  const bool config_loaded = config_manager_load(&config);
  if (!config_loaded || strlen(config.device_name) == 0) {
    String default_name = config_manager_get_default_device_name();
    strlcpy(config.device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
  }
  rtc_image_state_init();

  const uint32_t sleep_seconds = config.sleep_timeout_seconds > 0
      ? config.sleep_timeout_seconds
      : kDefaultSleepSeconds;

  const uint16_t long_press_ms = config.long_press_ms > 0 ? config.long_press_ms : kDefaultLongPressMs;
  if (config.always_on) {
    run_always_on(config, config_loaded);
    return;
  } else {
    const bool long_press = check_long_press(long_press_ms);
    if (long_press) {
      LOGI("Button", "Long press detected (%ums) - config mode requested", (unsigned)long_press_ms);
      run_config_mode(config, config_loaded);
      return;
    }
  }

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
    enter_deep_sleep(sleep_seconds);
    return;
  }
  LOG_DURATION("SD", "Init", sd_start);

  randomSeed(analogRead(0));

  char g4_path[64];
  const SdImageSelectMode mode = (strcmp(config.image_selection_mode, "sequential") == 0)
      ? SdImageSelectMode::Sequential
      : SdImageSelectMode::Random;
  const uint32_t last_index = rtc_image_state_get_last_image_index();
  const char *last_name = rtc_image_state_get_last_image_name();
  uint32_t selected_index = 0;
  char selected_name[64] = {0};
  if (!sd_pick_g4_image(g4_path, sizeof(g4_path), mode, last_index, last_name, &selected_index, selected_name, sizeof(selected_name))) {
    LOGW("SD", "No .g4 files found");
    enter_deep_sleep(sleep_seconds);
    return;
  }

  const unsigned long disp_start = millis();
  if (!it8951_renderer_init()) {
    LOGE("EINK", "Init failed");
    enter_deep_sleep(sleep_seconds);
    return;
  }
  LOG_DURATION("EINK", "Init", disp_start);

  LOGI("EINK", "Render G4=%s", g4_path);
  if (!it8951_render_g4(g4_path)) {
    LOGE("EINK", "Render G4 failed");
  } else if (mode == SdImageSelectMode::Sequential) {
    rtc_image_state_set_last_image_index(selected_index);
    rtc_image_state_set_last_image_name(selected_name);
  }

  it8951_renderer_hibernate();
  LOGI("Sleep", "Sleep %lus", (unsigned long)sleep_seconds);
  enter_deep_sleep(sleep_seconds);
}

void loop() {}