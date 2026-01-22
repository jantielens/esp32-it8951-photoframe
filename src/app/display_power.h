#pragma once

#include <Arduino.h>

// Display power control (MiniBoost EN -> IT8951 5V rail)
//
// If DISPLAY_POWER_EN_PIN is defined and >= 0, firmware can enable/disable the
// boost converter. For deep sleep battery life, we also latch the pin level
// using ESP-IDF GPIO hold where supported.

void display_power_init();
void display_power_on();
void display_power_prepare_for_sleep();
