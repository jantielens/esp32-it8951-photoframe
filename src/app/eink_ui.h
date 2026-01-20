#pragma once

#include "display_driver.h"

#include <Arduino.h>
#include <Adafruit_GFX.h>

class EInkCanvas1 : public Adafruit_GFX {
public:
    EInkCanvas1(uint16_t w, uint16_t h);
    ~EInkCanvas1() override;

    bool begin();
    void clear(bool white = true);
    uint8_t* data() { return buffer; }
    const uint8_t* data() const { return buffer; }
    size_t dataSize() const { return bufferBytes; }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override;

private:
    uint8_t* buffer;
    size_t bufferBytes;
};

class EInkUi {
public:
    EInkUi();
    ~EInkUi();

    bool init(DisplayDriver* displayDriver);

    void setTitle(const char* text);
    void setStatus(const char* text);
    void setProgress(int percent); // 0-100, or -1 to hide
    void clearProgress();

    bool render(bool fullRefresh);

private:
    void redraw();
    bool ensureBuffers();
    void convertToG4();

    DisplayDriver* driver;
    EInkCanvas1* canvas;
    uint8_t* g4Buffer;
    size_t g4BufferBytes;
    uint16_t width;
    uint16_t height;

    char title[64];
    char status[96];
    int progress;
};
