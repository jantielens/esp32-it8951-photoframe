#include "input_manager.h"

#include "log_manager.h"

#include <soc/soc_caps.h>
#if SOC_TOUCH_SENSOR_SUPPORTED
#include <driver/touch_pad.h>
#endif

namespace {
static int g_button_pin = -1;
static uint8_t g_button_active_level = LOW;
static uint32_t g_button_debounce_ms = 30;
static constexpr float kTouchWakeThresholdRatio = 0.005f;

static bool is_button_pressed() {
    if (g_button_pin < 0) return false;
    return digitalRead(g_button_pin) == g_button_active_level;
}


static bool poll_button_click() {
    if (g_button_pin < 0) return false;
    static bool last_read = false;
    static bool stable_state = false;
    static unsigned long last_change_ms = 0;

    const unsigned long now = millis();
    const bool pressed = is_button_pressed();

    if (pressed != last_read) {
        last_read = pressed;
        last_change_ms = now;
    }

    if ((now - last_change_ms) >= g_button_debounce_ms && stable_state != last_read) {
        stable_state = last_read;
        if (stable_state) {
            LOGI("Input", "Button click debounce=%lums", (unsigned long)g_button_debounce_ms);
            return true;
        }
    }

    return false;
}
}

void input_manager_init(
    int button_pin,
    uint8_t button_active_level,
    uint32_t button_debounce_ms
) {
    g_button_pin = button_pin;
    g_button_active_level = button_active_level;
    g_button_debounce_ms = button_debounce_ms;

    if (g_button_pin >= 0) {
        pinMode(g_button_pin, INPUT_PULLUP);
    }

    LOGI("Input", "Init button_pin=%d", g_button_pin);
}

bool input_manager_check_long_press(uint16_t long_press_ms) {
    if (!is_button_pressed()) return false;
    const unsigned long start_ms = millis();
    while (millis() - start_ms < long_press_ms) {
        if (!is_button_pressed()) return false;
        delay(10);
    }
    LOGI("Input", "Long press detected (%u ms)", (unsigned)long_press_ms);
    return true;
}

void input_manager_poll(InputEvents &events) {
    events.button_click = poll_button_click();
    if (events.button_click) {
        LOGI("Input", "Button click");
    }
}

void input_manager_enable_touch_wakeup(uint8_t touch_gpio, const TouchWakeConfig &config) {
#if SOC_TOUCH_SENSOR_SUPPORTED
    if (touch_gpio >= SOC_TOUCH_SENSOR_NUM) {
        LOGW("Input", "Touch wake not configured: GPIO%u out of range (max %u)",
             (unsigned)touch_gpio, (unsigned)SOC_TOUCH_SENSOR_NUM);
        return;
    }

    const touch_pad_t touch_pad = static_cast<touch_pad_t>(touch_gpio);
    touch_pad_init();
    touch_pad_config(touch_pad);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();

    uint32_t min_sample = 0;
    const uint8_t total_samples = config.samples > 0 ? config.samples : 1;
    const uint8_t discard = (config.discard_first >= total_samples) ? (total_samples - 1) : config.discard_first;

    for (uint8_t i = 0; i < total_samples; i++) {
        uint32_t sample = 0;
        if (touch_pad_read_raw_data(touch_pad, &sample) == ESP_OK) {
            if (i >= discard) {
                if (min_sample == 0 || sample < min_sample) {
                    min_sample = sample;
                }
            }
        }
        delay(config.sample_delay_ms);
    }

    const uint32_t baseline = min_sample;
    const float ratio = config.threshold_percent ? (config.threshold_percent / 100.0f) : kTouchWakeThresholdRatio;
    uint32_t threshold = baseline + (uint32_t)(baseline * ratio);
    if (threshold <= baseline) {
        threshold = baseline + 1;
    }

    touch_pad_set_thresh(touch_pad, threshold);
    esp_sleep_enable_touchpad_wakeup();
    LOGI("Input", "Touch wake enabled on GPIO%u (baseline=%u threshold=%u)",
         (unsigned)touch_gpio, (unsigned)baseline, (unsigned)threshold);
#else
    (void)touch_gpio;
    (void)config;
    LOGW("Input", "Touch wake not supported on this target");
#endif
}
