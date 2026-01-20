#include "input_manager.h"

#include "log_manager.h"

namespace {
static int g_button_pin = -1;
static uint8_t g_button_active_level = LOW;
static uint32_t g_button_debounce_ms = 30;

static uint8_t g_touch_gpio = 0;
static uint8_t g_touch_samples = 8;
static float g_touch_threshold_ratio = 1.3f;
static uint32_t g_touch_debounce_ms = 250;

static bool g_touch_ready = false;
static uint32_t g_touch_baseline = 0;
static uint32_t g_touch_threshold = 0;
static bool g_touch_active = false;
static unsigned long g_touch_last_trigger_ms = 0;

static bool is_button_pressed() {
    if (g_button_pin < 0) return false;
    return digitalRead(g_button_pin) == g_button_active_level;
}

static bool touch_read_value(uint32_t *out_value) {
    if (!out_value) return false;
    *out_value = (uint32_t)touchRead(g_touch_gpio);
    return true;
}

static bool init_touch_pad() {
    g_touch_ready = false;
    g_touch_baseline = 0;
    g_touch_threshold = 0;

    uint32_t sum = 0;
    uint8_t samples = 0;
    bool skipped_first = false;
    for (uint8_t i = 0; i < g_touch_samples; i++) {
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

    g_touch_baseline = sum / samples;
    g_touch_threshold = (uint32_t)((float)g_touch_baseline * g_touch_threshold_ratio);

    g_touch_ready = true;
    LOGI("Touch", "Calibrated baseline=%lu threshold=%lu (ratio=%.2f) gpio=%u",
         (unsigned long)g_touch_baseline,
         (unsigned long)g_touch_threshold,
         g_touch_threshold_ratio,
         g_touch_gpio);
    return true;
}

static bool poll_touch_trigger() {
    if (!g_touch_ready) return false;

    uint32_t value = 0;
    if (!touch_read_value(&value)) return false;

    const unsigned long now = millis();
    const bool touched = (value > g_touch_threshold);
    if (touched && !g_touch_active && (now - g_touch_last_trigger_ms) > g_touch_debounce_ms) {
        g_touch_active = true;
        g_touch_last_trigger_ms = now;
        LOGI("Touch", "Trigger value=%lu threshold=%lu", (unsigned long)value, (unsigned long)g_touch_threshold);
        return true;
    }

    if (!touched && g_touch_active) {
        g_touch_active = false;
    }

    return false;
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
    uint32_t button_debounce_ms,
    uint8_t touch_gpio,
    uint8_t touch_samples,
    float touch_threshold_ratio,
    uint32_t touch_debounce_ms
) {
    g_button_pin = button_pin;
    g_button_active_level = button_active_level;
    g_button_debounce_ms = button_debounce_ms;
    g_touch_gpio = touch_gpio;
    g_touch_samples = touch_samples;
    g_touch_threshold_ratio = touch_threshold_ratio;
    g_touch_debounce_ms = touch_debounce_ms;

    if (g_button_pin >= 0) {
        pinMode(g_button_pin, INPUT_PULLUP);
    }

    LOGI("Input", "Init button_pin=%d touch_gpio=%u", g_button_pin, g_touch_gpio);
    init_touch_pad();
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
    events.touch_trigger = poll_touch_trigger();
    if (events.button_click) {
        LOGI("Input", "Button click");
    }
    if (events.touch_trigger) {
        LOGI("Input", "Touch trigger");
    }
}

bool input_manager_is_touch_ready() {
    return g_touch_ready;
}

uint32_t input_manager_get_touch_baseline() {
    return g_touch_baseline;
}

uint32_t input_manager_get_touch_threshold() {
    return g_touch_threshold;
}
