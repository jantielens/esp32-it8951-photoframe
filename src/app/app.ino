#include "board_config.h"
#include "it8951_renderer.h"
#include "log_manager.h"
#include "config_manager.h"
#include "rtc_state.h"
#include "device_telemetry.h"
#include "input_manager.h"
#include "display_power.h"
#include "blob_pull.h"
#include "portal_controller.h"
#include "render_scheduler.h"
#include "boot_mode.h"
#if HAS_MQTT
#include "mqtt_manager.h"
#endif
#include "display_manager.h"
#if HEALTH_HISTORY_ENABLED
#include "health_history.h"
#endif

#include <SPI.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <time.h>
static constexpr uint32_t kDefaultSleepSeconds = 60;
static constexpr uint32_t kSdFrequencyHz = 80000000;
static constexpr uint16_t kDefaultLongPressMs = 1500;
static constexpr uint8_t kButtonActiveLevel = LOW;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint8_t kTouchGpio = 6; // TOUCH06 -> GPIO6 on ESP32-S2
static constexpr uint32_t kNoImageRetryMs = 5000;
static constexpr time_t kValidEpochThreshold = 1609459200; // 2021-01-01T00:00:00Z

static SPIClass sdSpi(HSPI);

static const SdCardPins kSdPins = {
  .cs = SD_CS_PIN,
  .sck = SD_SCK_PIN,
  .miso = SD_MISO_PIN,
  .mosi = SD_MOSI_PIN,
  .power = SD_POWER_PIN,
};

#if HAS_MQTT
MqttManager mqtt_manager;
#endif

struct BlobPullContext {
  const DeviceConfig *config = nullptr;
  SPIClass *spi = nullptr;
  SdCardPins pins = {};
  uint32_t frequency_hz = 0;
};

static BlobPullContext g_blob_pull_ctx = {};

static bool blob_pull_pre_enqueue(void *ctx) {
  BlobPullContext *state = static_cast<BlobPullContext *>(ctx);
  if (!state || !state->config || !state->spi) return false;
  if (strlen(state->config->blob_sas_url) == 0) return false;
  LOGI("Blob", "Scheduler hook: attempting blob download");
  return blob_pull_download_once(*state->config, *state->spi, state->pins, state->frequency_hz);
}

// Best-effort NTP sync used for temp expiry cleanup. If it fails, we continue
// rendering without deletion.
static bool sync_time_ntp(bool wifi_connected, bool show_status) {
  if (!wifi_connected) return false;

  LOGI("Time", "NTP sync start");
  if (show_status) {
    display_manager_set_splash_status("Syncing time...");
    display_manager_render_now();
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  const unsigned long start = millis();
  time_t now = 0;
  while (millis() - start < 5000) {
    time(&now);
    if (now >= kValidEpochThreshold) {
      LOGI("Time", "NTP sync OK (%lu)", (unsigned long)now);
      return true;
    }
    delay(100);
  }

  LOGW("Time", "NTP sync failed (timeout)");
  if (show_status) {
    display_manager_set_splash_status("Time sync failed");
    display_manager_render_now();
  }
  return false;
}

static void wifi_shutdown() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(true);
    delay(50);
  }
  WiFi.mode(WIFI_OFF);
}

#if HAS_MQTT
static void mqtt_init_from_config(const DeviceConfig &config) {
  char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
  config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
  mqtt_manager.begin(&config, config.device_name, sanitized);
}

static void mqtt_publish_before_sleep(const DeviceConfig &config, bool wifi_connected) {
  if (strlen(config.mqtt_host) == 0) return;
  if (!wifi_connected) {
    LOGW("MQTT", "WiFi not connected; skipping publish");
    return;
  }

  mqtt_init_from_config(config);

  const unsigned long start = millis();
  bool connected = false;
  while (millis() - start < 5000) {
    mqtt_manager.loop();
    if (mqtt_manager.connected()) {
      connected = true;
      break;
    }
    delay(100);
  }

  if (connected) {
    const unsigned long publish_start = millis();
    while (millis() - publish_start < 1000) {
      mqtt_manager.loop();
      delay(50);
    }
  } else {
    LOGW("MQTT", "Connect timeout before sleep");
  }
}
#endif

static void enter_deep_sleep(uint32_t sleep_seconds) {
  Serial.flush();
  delay(200);
  LOGI("Sleep", "Entering deep sleep for %lus", (unsigned long)sleep_seconds);

  // Ensure IT8951 SPI/control pins are high-Z before removing the 5V rail.
  it8951_renderer_prepare_for_power_cut();

  // Keep the IT8951 5V rail disabled during deep sleep.
  display_power_prepare_for_sleep();

  esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);

  // Wake buttons: support one or more physical buttons using EXT1.
  // Wiring assumption: button shorts GPIO -> GND when pressed (active LOW).
  auto prep_wakeup_pin_active_low = [](int pin) {
    if (pin < 0) return;
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    rtc_gpio_deinit(gpio);
    rtc_gpio_pullup_en(gpio);
    rtc_gpio_pulldown_dis(gpio);
  };

  uint64_t ext1_mask = 0;
  if (BUTTON_PIN >= 0) {
    prep_wakeup_pin_active_low(BUTTON_PIN);
    ext1_mask |= (1ULL << BUTTON_PIN);
  }
  #if defined(WAKE_BUTTON2_PIN)
  if (WAKE_BUTTON2_PIN >= 0) {
    prep_wakeup_pin_active_low(WAKE_BUTTON2_PIN);
    ext1_mask |= (1ULL << WAKE_BUTTON2_PIN);
  }
  #endif

  if (ext1_mask != 0) {
    const esp_sleep_ext1_wakeup_mode_t mode = (kButtonActiveLevel == LOW)
        ? ESP_EXT1_WAKEUP_ANY_LOW
        : ESP_EXT1_WAKEUP_ANY_HIGH;
    esp_sleep_enable_ext1_wakeup(ext1_mask, mode);
  }

  TouchWakeConfig touch_config;
  input_manager_enable_touch_wakeup(kTouchGpio, touch_config);
  esp_deep_sleep_start();
}

static BootDecision decide_boot_mode(const DeviceConfig &config, esp_sleep_wakeup_cause_t wake_cause, bool long_press) {
  BootDecision d;

  if (config.always_on) {
    d.mode = BootMode::AlwaysOn;
    d.quiet_ui = false;
    return d;
  }

  if (long_press) {
    d.mode = BootMode::ConfigPortal;
    d.quiet_ui = false;
    return d;
  }

  d.mode = BootMode::SleepCycle;
  d.quiet_ui = is_quiet_wake_cause(wake_cause);
  return d;
}

static void run_config_mode(DeviceConfig &config, bool config_loaded) {
  LOGI("Portal", "Config mode start");

  display_manager_set_splash_status("AP mode: configure WiFi");
  display_manager_render_now();

  portal_controller_start(config, config_loaded, sdSpi, kSdPins, kSdFrequencyHz);

  while (true) {
    portal_controller_tick();
    if (portal_controller_is_paused()) {
      delay(10);
      continue;
    }
    delay(10);
  }
}

static void run_always_on(DeviceConfig &config, bool config_loaded) {
  LOGI("Mode", "Always-on enabled");

  display_manager_set_splash_status("Always-on mode");
  display_manager_render_now();

  portal_controller_start(config, config_loaded, sdSpi, kSdPins, kSdFrequencyHz);

  // Keep time reasonably fresh for temp expiry cleanup while always-on.
  sync_time_ntp(WiFi.status() == WL_CONNECTED, false);

  #if HAS_MQTT
  mqtt_init_from_config(config);
  #endif

  const uint32_t sleep_seconds = config.sleep_timeout_seconds > 0
      ? config.sleep_timeout_seconds
      : kDefaultSleepSeconds;
  const uint32_t refresh_interval_ms = sleep_seconds * 1000UL;

    LOGI("Render", "Scheduler init refresh=%lums retry=%lums",
      (unsigned long)refresh_interval_ms,
      (unsigned long)kNoImageRetryMs);
    render_scheduler_init(config, refresh_interval_ms, kNoImageRetryMs);

    g_blob_pull_ctx.config = &config;
    g_blob_pull_ctx.spi = &sdSpi;
    g_blob_pull_ctx.pins = kSdPins;
    g_blob_pull_ctx.frequency_hz = kSdFrequencyHz;
    render_scheduler_set_pre_enqueue_hook(blob_pull_pre_enqueue, &g_blob_pull_ctx);

  while (true) {
    portal_controller_tick();

    InputEvents events = {};
    input_manager_poll(events);
    if (events.button_click) {
      LOGI("Input", "Button click -> refresh");
      render_scheduler_request_refresh();
    }

    render_scheduler_tick();

    #if HAS_MQTT
    mqtt_manager.loop();
    #endif

    delay(10);
  }
}

// One-shot sleep cycle: never starts the portal.
static void run_sleep_cycle(const DeviceConfig &config, uint32_t sleep_seconds, bool quiet_ui) {
  LOGI("Mode", "SleepCycle start (quiet_ui=%s)", quiet_ui ? "true" : "false");

  const bool wifi_was_connected = (WiFi.status() == WL_CONNECTED);
  bool wifi_connected = wifi_was_connected;

  const bool needs_wifi = (strlen(config.blob_sas_url) > 0)
  #if HAS_MQTT
      || (strlen(config.mqtt_host) > 0)
  #endif
      ;

  if (!wifi_connected && needs_wifi) {
    wifi_connected = wifi_connect_fast_sleepcycle(config, "Boot", /*budget_ms=*/6000, /*show_status=*/!quiet_ui);
  }

  // Best-effort; used for temp expiry cleanup.
  sync_time_ntp(wifi_connected, !quiet_ui);

  bool downloaded = false;
  if (strlen(config.blob_sas_url) > 0) {
    LOGI("Blob", "SAS configured; attempting blob pull");
    if (wifi_connected) {
      if (!quiet_ui) {
        display_manager_set_splash_status("Downloading image...");
        display_manager_render_now();
      }
      downloaded = blob_pull_download_once(config, sdSpi, kSdPins, kSdFrequencyHz);
      if (!quiet_ui) {
        display_manager_set_splash_status(downloaded ? "Download OK" : "Download failed");
        display_manager_render_now();
      }
    } else {
      LOGW("Blob", "WiFi unavailable; skipping blob pull");
    }
    if (!downloaded) {
      LOGW("Blob", "No blob downloaded; falling back to SD");
    }
  }

  if (!quiet_ui) {
    display_manager_set_splash_status("Rendering...");
    display_manager_render_now();
  }

  if (!render_scheduler_render_once(config, sdSpi, kSdPins, kSdFrequencyHz)) {
    LOGW("Mode", "Render once failed; entering deep sleep");
    #if HAS_MQTT
    mqtt_publish_before_sleep(config, wifi_connected);
    #endif
    if (!wifi_was_connected) {
      wifi_shutdown();
    }
    enter_deep_sleep(sleep_seconds);
    return;
  }

  #if HAS_MQTT
  mqtt_publish_before_sleep(config, wifi_connected);
  #endif

  if (!wifi_was_connected) {
    wifi_shutdown();
  }

  it8951_renderer_hibernate();
  enter_deep_sleep(sleep_seconds);
}

void setup() {
  #if HEALTH_HISTORY_ENABLED
  health_history_start();
  #endif

  log_init(115200);
  delay(200);
  LOGI("Boot", "Boot");
  const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  LOGI("Boot", "Wake cause=%d", (int)wake_cause);

  // Ensure display boost EN is in a known state early.
  display_power_init();

  input_manager_init(
      BUTTON_PIN,
      kButtonActiveLevel,
      kButtonDebounceMs
  );

  device_telemetry_init();
  device_telemetry_start_cpu_monitoring();
  device_telemetry_start_health_window_sampling();

  DeviceConfig config = {};

  config_manager_init();
  // Display is always present; UI verbosity is controlled by quiet_ui.
  const bool config_loaded = config_manager_load(&config);
  if (!config_loaded || strlen(config.device_name) == 0) {
    String default_name = config_manager_get_default_device_name();
    strlcpy(config.device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
  }
  rtc_image_state_init();

  const uint16_t long_press_ms = config.long_press_ms > 0 ? config.long_press_ms : kDefaultLongPressMs;
  const bool long_press = (long_press_ms > 0) ? input_manager_check_long_press(long_press_ms) : false;

  const uint32_t sleep_seconds = config.sleep_timeout_seconds > 0
      ? config.sleep_timeout_seconds
      : kDefaultSleepSeconds;

  const BootDecision decision = decide_boot_mode(config, wake_cause, long_press);

  LOGI("Boot", "Config loaded=%s always_on=%s sleep=%lus wifi=%s image_mode=%s long_press=%s mode=%s quiet_ui=%s",
      config_loaded ? "true" : "false",
      config.always_on ? "true" : "false",
      (unsigned long)sleep_seconds,
      strlen(config.wifi_ssid) > 0 ? "set" : "empty",
      config.image_selection_mode,
      long_press ? "true" : "false",
      boot_mode_name(decision.mode),
      decision.quiet_ui ? "true" : "false");

  if (!decision.quiet_ui) {
    display_manager_init(&config);
    {
      char status[64];
      snprintf(status, sizeof(status), "Display OK %dx%d r%d", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ROTATION);
      display_manager_set_splash_status(status);
      display_manager_render_full_now();
    }
  }

  switch (decision.mode) {
    case BootMode::AlwaysOn:
      LOGI("Mode", "Always-on selected");
      run_always_on(config, config_loaded);
      return;

    case BootMode::ConfigPortal:
      LOGI("Button", "Long press detected (%ums) - config mode requested", (unsigned)long_press_ms);
      run_config_mode(config, config_loaded);
      return;

    case BootMode::SleepCycle:
      run_sleep_cycle(config, sleep_seconds, decision.quiet_ui);
      return;
  }
}

void loop() {}