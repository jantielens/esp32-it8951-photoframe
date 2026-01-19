#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

struct SdCardPins {
    int cs;
    int sck;
    int miso;
    int mosi;
    int power;
};

bool sd_photo_picker_init(SPIClass &spi, const SdCardPins &pins, uint32_t frequency_hz);
bool sd_pick_random_bmp(char *out_path, size_t out_len);

enum class SdImageSelectMode : uint8_t {
    Random = 0,
    Sequential = 1,
};

// Select a .g4 image from SD root.
// last_index is the last displayed index for sequential mode (use UINT32_MAX when unknown).
// out_selected_index returns the index of the chosen image within the sorted list.
bool sd_pick_g4_image(
    char *out_path,
    size_t out_len,
    SdImageSelectMode mode,
    uint32_t last_index,
    uint32_t *out_selected_index
);
