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
