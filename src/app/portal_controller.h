#pragma once

#include "config_manager.h"
#include "sd_photo_picker.h"
#include <SPI.h>

void portal_controller_start(DeviceConfig &config, bool config_loaded, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz);
void portal_controller_tick();
bool portal_controller_is_paused();
