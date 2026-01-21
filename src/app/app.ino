#include "board_config.h"
#include "it8951_renderer.h"
#include "log_manager.h"
#include "config_manager.h"
#include "rtc_state.h"
#include "device_telemetry.h"
#include "input_manager.h"
#include "blob_pull.h"
#include "portal_controller.h"
#include "render_scheduler.h"
#if HAS_MQTT
#include "mqtt_manager.h"
#endif
#if HAS_DISPLAY
#include "display_manager.h"
#endif
#if HEALTH_HISTORY_ENABLED
#include "health_history.h"
#endif

#include <SPI.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
static constexpr uint32_t kDefaultSleepSeconds = 60;
static constexpr uint32_t kSdFrequencyHz = 80000000;
static constexpr uint16_t kDefaultLongPressMs = 1500;
static constexpr uint8_t kButtonActiveLevel = LOW;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint8_t kTouchGpio = 6; // TOUCH06 -> GPIO6 on ESP32-S2
static constexpr uint32_t kNoImageRetryMs = 5000;

static SPIClass sdSpi(HSPI);

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

static bool connect_wifi_sta(const DeviceConfig &config, const char *reason, bool show_status) {
  if (strlen(config.wifi_ssid) == 0) return false;

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  #if HAS_DISPLAY
  if (show_status) {
    display_manager_set_splash_status("Connecting to WiFi...");
    display_manager_render_now();
  }
  #endif

  LOGI("WiFi", "%s: connect start (ssid set)", reason ? reason : "WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifi_ssid, config.wifi_password);

  for (int attempt = 0; attempt < WIFI_MAX_ATTEMPTS; attempt++) {
    const unsigned long start = millis();
    while (millis() - start < 3000) {
      if (WiFi.status() == WL_CONNECTED) {
        #if HAS_DISPLAY
        if (show_status) {
          display_manager_set_splash_status("WiFi connected");
          display_manager_render_now();
        }
        #endif
        LOGI("WiFi", "%s: connected %s", reason ? reason : "WiFi", WiFi.localIP().toString().c_str());
        return true;
      }
      delay(100);
    }
    LOGW("WiFi", "%s: connect attempt %d/%d failed", reason ? reason : "WiFi", attempt + 1, WIFI_MAX_ATTEMPTS);
  }

  LOGW("WiFi", "%s: connect failed (max attempts)", reason ? reason : "WiFi");
  #if HAS_DISPLAY
  if (show_status) {
    display_manager_set_splash_status("WiFi connect failed");
    display_manager_render_now();
  }
  #endif
  return false;
}

static void disconnect_wifi_for_mqtt() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(true);
    delay(50);
  }
  WiFi.mode(WIFI_OFF);
}

static SdCardPins make_sd_pins() {
  SdCardPins pins = {
      .cs = SD_CS_PIN,
      .sck = SD_SCK_PIN,
      .miso = SD_MISO_PIN,
      .mosi = SD_MOSI_PIN,
      .power = SD_POWER_PIN,
  };
  return pins;
}

#if HAS_MQTT
static void mqtt_init_from_config(const DeviceConfig &config) {
  char sanitized[CONFIG_DEVICE_NAME_MAX_LEN];
  config_manager_sanitize_device_name(config.device_name, sanitized, sizeof(sanitized));
  mqtt_manager.begin(&config, config.device_name, sanitized);
}

static void mqtt_publish_before_sleep(const DeviceConfig &config, bool wifi_connected, bool disconnect_after) {
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

  if (disconnect_after) {
    disconnect_wifi_for_mqtt();
  }
}
#endif

static void enter_deep_sleep(uint32_t sleep_seconds) {
  Serial.flush();
  delay(200);
  LOGI("Sleep", "Entering deep sleep for %lus", (unsigned long)sleep_seconds);
  esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);
  if (BUTTON_PIN >= 0) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(BUTTON_PIN));
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(BUTTON_PIN));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(BUTTON_PIN));
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BUTTON_PIN), kButtonActiveLevel);
  }
  TouchWakeConfig touch_config;
  input_manager_enable_touch_wakeup(kTouchGpio, touch_config);
  esp_deep_sleep_start();
}

// Fast-wake path: for timer or button wake when always-on is disabled.
// Goal: render next image and sleep ASAP (skip splash + portal + WiFi).
static bool is_fast_wake(const DeviceConfig &config, uint16_t long_press_ms) {
  if (config.always_on) return false;
  if (long_press_ms > 0 && input_manager_check_long_press(long_press_ms)) {
    return false;  // Long press should enter config mode.
  }

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  return (cause == ESP_SLEEP_WAKEUP_TIMER ||
      cause == ESP_SLEEP_WAKEUP_EXT0 ||
      cause == ESP_SLEEP_WAKEUP_TOUCHPAD);
}

static void run_config_mode(DeviceConfig &config, bool config_loaded) {
  LOGI("Portal", "Config mode start");

  #if HAS_DISPLAY
  display_manager_set_splash_status("AP mode: configure WiFi");
  display_manager_render_now();
  #endif

  const SdCardPins pins = make_sd_pins();
  portal_controller_start(config, config_loaded, sdSpi, pins, kSdFrequencyHz);

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

  #if HAS_DISPLAY
  display_manager_set_splash_status("Always-on mode");
  display_manager_render_now();
  #endif

  const SdCardPins pins = make_sd_pins();
  portal_controller_start(config, config_loaded, sdSpi, pins, kSdFrequencyHz);

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
    g_blob_pull_ctx.pins = pins;
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

void setup() {
  #if HEALTH_HISTORY_ENABLED
  health_history_start();
  #endif

  log_init(115200);
  delay(200);
  LOGI("Boot", "Boot");
  LOGI("Boot", "Wake cause=%d", (int)esp_sleep_get_wakeup_cause());

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
  #if HAS_DISPLAY
  // Avoid UI work until we decide whether to take the fast-wake path.
  #endif
  const bool config_loaded = config_manager_load(&config);
  if (!config_loaded || strlen(config.device_name) == 0) {
    String default_name = config_manager_get_default_device_name();
    strlcpy(config.device_name, default_name.c_str(), CONFIG_DEVICE_NAME_MAX_LEN);
  }
  rtc_image_state_init();

    LOGI("Boot", "Config loaded=%s always_on=%s sleep=%lus wifi=%s mode=%s",
      config_loaded ? "true" : "false",
      config.always_on ? "true" : "false",
      (unsigned long)(config.sleep_timeout_seconds > 0 ? config.sleep_timeout_seconds : kDefaultSleepSeconds),
      strlen(config.wifi_ssid) > 0 ? "set" : "empty",
      config.image_selection_mode);

  const uint16_t long_press_ms = config.long_press_ms > 0 ? config.long_press_ms : kDefaultLongPressMs;
  const bool fast_wake = is_fast_wake(config, long_press_ms);

  #if HAS_DISPLAY
  if (!fast_wake) {
    display_manager_init(&config);
    {
      char status[64];
      snprintf(status, sizeof(status), "Display OK %dx%d r%d", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_ROTATION);
      display_manager_set_splash_status(status);
      display_manager_render_full_now();
    }
    display_manager_set_splash_status("Booting...");
    display_manager_render_now_ex(false);
    display_manager_set_splash_status("Loading config...");
    display_manager_render_now();
    display_manager_set_splash_status(config_loaded ? "Config loaded" : "Using defaults");
    display_manager_render_now();
  }
  #endif

  const uint32_t sleep_seconds = config.sleep_timeout_seconds > 0
      ? config.sleep_timeout_seconds
      : kDefaultSleepSeconds;
  if (config.always_on) {
    LOGI("Mode", "Always-on selected");
#if HAS_DISPLAY
    display_manager_set_splash_status("Start rendering image");
    display_manager_render_now();
#endif
    run_always_on(config, config_loaded);
    return;
  } else {
    if (!fast_wake) {
      const bool long_press = input_manager_check_long_press(long_press_ms);
      if (long_press) {
        LOGI("Button", "Long press detected (%ums) - config mode requested", (unsigned)long_press_ms);
        run_config_mode(config, config_loaded);
        return;
      }
    }
  }

  LOGI("Mode", "Default sleep mode selected");

  const SdCardPins pins = make_sd_pins();

  bool wifi_was_connected = (WiFi.status() == WL_CONNECTED);
  bool wifi_connected = wifi_was_connected;
  const bool needs_wifi = (strlen(config.blob_sas_url) > 0)
  #if HAS_MQTT
      || (strlen(config.mqtt_host) > 0)
  #endif
      ;

  if (!wifi_connected && needs_wifi) {
    const bool show_status = !fast_wake;
    wifi_connected = connect_wifi_sta(config, "Boot", show_status);
  }

  bool downloaded = false;
  if (strlen(config.blob_sas_url) > 0) {
    LOGI("Blob", "SAS configured; attempting blob pull");
    if (wifi_connected) {
      downloaded = blob_pull_download_once(config, sdSpi, pins, kSdFrequencyHz);
    } else {
      LOGW("Blob", "WiFi unavailable; skipping blob pull");
    }
    if (!downloaded) {
      LOGW("Blob", "No blob downloaded; falling back to SD");
    }
  }

  if (!render_scheduler_render_once(config, sdSpi, pins, kSdFrequencyHz)) {
    LOGW("Mode", "Render once failed; entering deep sleep");
    #if HAS_MQTT
    mqtt_publish_before_sleep(config, wifi_connected, !wifi_was_connected);
    #endif
    enter_deep_sleep(sleep_seconds);
    return;
  }

  #if HAS_MQTT
  mqtt_publish_before_sleep(config, wifi_connected, !wifi_was_connected);
  #endif
  it8951_renderer_hibernate();
  enter_deep_sleep(sleep_seconds);
}

void loop() {}