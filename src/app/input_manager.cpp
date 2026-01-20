#include "input_manager.h"

#include "log_manager.h"

namespace {
static int g_button_pin = -1;
static uint8_t g_button_active_level = LOW;
static uint32_t g_button_debounce_ms = 30;

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
