#include "display_power.h"

#include "board_config.h"
#include "log_manager.h"

#include <driver/gpio.h>

static inline bool has_display_power_pin() {
#if defined(DISPLAY_POWER_EN_PIN)
    return DISPLAY_POWER_EN_PIN >= 0;
#else
    return false;
#endif
}

static inline gpio_num_t power_gpio() {
    return static_cast<gpio_num_t>(DISPLAY_POWER_EN_PIN);
}

static inline void disable_holds_if_possible() {
#if defined(DISPLAY_POWER_EN_PIN)
    // Release any previous hold state.
    gpio_hold_dis(power_gpio());
    gpio_deep_sleep_hold_dis();
#endif
}

void display_power_init() {
    if (!has_display_power_pin()) return;

    disable_holds_if_possible();

    pinMode(DISPLAY_POWER_EN_PIN, OUTPUT);
    digitalWrite(DISPLAY_POWER_EN_PIN, LOW);

    // Do not hold here: we want the pin to be freely controllable during active mode.
    LOGI("PWR", "Display power EN on GPIO%d (default OFF)", (int)DISPLAY_POWER_EN_PIN);
}

void display_power_on() {
    if (!has_display_power_pin()) return;

    disable_holds_if_possible();

    pinMode(DISPLAY_POWER_EN_PIN, OUTPUT);
    digitalWrite(DISPLAY_POWER_EN_PIN, HIGH);

    // Give the boost + HAT rail a moment to rise before SPI traffic.
    delay(15);
}

void display_power_prepare_for_sleep() {
    if (!has_display_power_pin()) return;

    // Force OFF.
    pinMode(DISPLAY_POWER_EN_PIN, OUTPUT);
    digitalWrite(DISPLAY_POWER_EN_PIN, LOW);

    // Latch level through deep sleep.
    gpio_hold_en(power_gpio());
    gpio_deep_sleep_hold_en();
}
