#pragma once

#include <Arduino.h>

struct InputEvents {
    bool button_click = false;
    bool touch_trigger = false;
};

void input_manager_init(
    int button_pin,
    uint8_t button_active_level,
    uint32_t button_debounce_ms,
    uint8_t touch_gpio,
    uint8_t touch_samples,
    float touch_threshold_ratio,
    uint32_t touch_debounce_ms
);

bool input_manager_check_long_press(uint16_t long_press_ms);
void input_manager_poll(InputEvents &events);

bool input_manager_is_touch_ready();
uint32_t input_manager_get_touch_baseline();
uint32_t input_manager_get_touch_threshold();
