#pragma once

#include "config_manager.h"
#include "sd_photo_picker.h"
#include <SPI.h>

void portal_controller_start(DeviceConfig &config, bool config_loaded, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz);
void portal_controller_tick();
bool portal_controller_is_paused();

// Shared WiFi helpers (used by both portal and sleep-cycle boot).
bool wifi_connect_fast_sleepcycle(const DeviceConfig &config, const char *reason, uint32_t budget_ms, bool show_status);
bool wifi_connect_robust_portal(const DeviceConfig &config, const char *reason, bool show_status);
void wifi_start_mdns(const DeviceConfig &config);
