#pragma once

#include <Arduino.h>

struct InputEvents {
    bool button_click = false;
};

void input_manager_init(
    int button_pin,
    uint8_t button_active_level,
    uint32_t button_debounce_ms
);

bool input_manager_check_long_press(uint16_t long_press_ms);
void input_manager_poll(InputEvents &events);
