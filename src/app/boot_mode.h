#pragma once

#include <esp_sleep.h>
#include <stdint.h>

enum class BootMode : uint8_t {
  AlwaysOn,
  ConfigPortal,
  SleepCycle,
};

struct BootDecision {
  BootMode mode = BootMode::SleepCycle;
  bool quiet_ui = false;
};

static inline const char *boot_mode_name(BootMode mode) {
  switch (mode) {
    case BootMode::AlwaysOn: return "AlwaysOn";
    case BootMode::ConfigPortal: return "ConfigPortal";
    case BootMode::SleepCycle: return "SleepCycle";
  }
  return "Unknown";
}

static inline bool is_quiet_wake_cause(esp_sleep_wakeup_cause_t cause) {
  return (cause == ESP_SLEEP_WAKEUP_TIMER ||
      cause == ESP_SLEEP_WAKEUP_EXT0 ||
      cause == ESP_SLEEP_WAKEUP_EXT1 ||
      cause == ESP_SLEEP_WAKEUP_TOUCHPAD);
}
