#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "config_manager.h"
#include "sd_photo_picker.h"

// Attempt to pull the next G4 image from Azure Blob storage and store it on SD.
// Returns true if an image was downloaded successfully.
bool blob_pull_download_once(const DeviceConfig &config, SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz);
