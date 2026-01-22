#pragma once

#include <Arduino.h>

struct InputEvents {
    bool button_click = false;
};

struct TouchWakeConfig {
    uint8_t samples = 5;
    uint8_t discard_first = 1;
    uint16_t sample_delay_ms = 20;
    uint8_t threshold_percent = 5;
};

void input_manager_init(
    int button_pin,
    uint8_t button_active_level,
    uint32_t button_debounce_ms,
    int button2_pin = -1
);

void input_manager_enable_touch_wakeup(uint8_t touch_gpio, const TouchWakeConfig &config);

bool input_manager_check_long_press(uint16_t long_press_ms);
void input_manager_poll(InputEvents &events);
