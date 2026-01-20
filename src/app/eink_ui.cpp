#include "eink_ui.h"

#include "board_config.h"
#include "log_manager.h"

#include <esp_heap_caps.h>
#include <string.h>

EInkCanvas1::EInkCanvas1(uint16_t w, uint16_t h)
    : Adafruit_GFX(w, h), buffer(nullptr), bufferBytes(0) {
}

EInkCanvas1::~EInkCanvas1() {
    if (buffer) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
}

bool EInkCanvas1::begin() {
    if (buffer) return true;

    bufferBytes = ((size_t)width() * (size_t)height() + 7) / 8;

    buffer = static_cast<uint8_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!buffer) {
        LOGE("UI", "Canvas alloc failed (%u bytes)", (unsigned)bufferBytes);
        return false;
    }

    clear(true);
    return true;
}

void EInkCanvas1::clear(bool white) {
    if (!buffer) return;
    memset(buffer, white ? 0xFF : 0x00, bufferBytes);
}

void EInkCanvas1::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (!buffer) return;
    if ((x < 0) || (y < 0) || (x >= (int16_t)width()) || (y >= (int16_t)height())) return;

    const uint32_t idx = (uint32_t)y * (uint32_t)width() + (uint32_t)x;
    const uint32_t byteIndex = idx >> 3;
    const uint8_t bitMask = (uint8_t)(0x80 >> (idx & 0x07));

    if (color) {
        buffer[byteIndex] |= bitMask;  // white
    } else {
        buffer[byteIndex] &= (uint8_t)(~bitMask); // black
    }
}

EInkUi::EInkUi()
    : driver(nullptr), canvas(nullptr), g4Buffer(nullptr), g4BufferBytes(0),
      width(0), height(0), progress(-1) {
    title[0] = '\0';
    status[0] = '\0';
}

EInkUi::~EInkUi() {
    if (g4Buffer) {
        heap_caps_free(g4Buffer);
        g4Buffer = nullptr;
    }
    delete canvas;
    canvas = nullptr;
}

bool EInkUi::init(DisplayDriver* displayDriver) {
    driver = displayDriver;
    if (!driver) return false;

    width = (uint16_t)driver->width();
    height = (uint16_t)driver->height();

    if (!ensureBuffers()) {
        return false;
    }

    return true;
}

void EInkUi::setTitle(const char* text) {
    strlcpy(title, text ? text : "", sizeof(title));
}

void EInkUi::setStatus(const char* text) {
    strlcpy(status, text ? text : "", sizeof(status));
}

void EInkUi::setProgress(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    progress = percent;
}

void EInkUi::clearProgress() {
    progress = -1;
}

bool EInkUi::render(bool fullRefresh) {
    if (!driver || !canvas || !g4Buffer) return false;

    redraw();
    convertToG4();

    return driver->presentG4Full(g4Buffer, fullRefresh);
}

bool EInkUi::ensureBuffers() {
    if (!canvas) {
        canvas = new EInkCanvas1(width, height);
        if (!canvas || !canvas->begin()) {
            delete canvas;
            canvas = nullptr;
            return false;
        }
    }

    if (!g4Buffer) {
        g4BufferBytes = (size_t)width * (size_t)height / 2;
        g4Buffer = static_cast<uint8_t*>(heap_caps_malloc(g4BufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!g4Buffer) {
            g4Buffer = static_cast<uint8_t*>(heap_caps_malloc(g4BufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!g4Buffer) {
            LOGE("UI", "G4 buffer alloc failed (%u bytes)", (unsigned)g4BufferBytes);
            return false;
        }
    }

    return true;
}

void EInkUi::redraw() {
    if (!canvas) return;

    canvas->clear(true);

    const int gap = 16;
    const int barHeight = 16;

    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t titleW = 0;
    uint16_t titleH = 0;
    uint16_t statusW = 0;
    uint16_t statusH = 0;

    canvas->setTextWrap(false);
    canvas->setTextColor(0, 1);

    canvas->setTextSize(8);
    canvas->getTextBounds(title, 0, 0, &x1, &y1, &titleW, &titleH);

    canvas->setTextSize(4);
    canvas->getTextBounds(status, 0, 0, &x1, &y1, &statusW, &statusH);

    const int blockH = (int)titleH + gap + (int)statusH + ((progress >= 0) ? (gap + barHeight) : 0);
    int top = ((int)height - blockH) / 2;
    if (top < 0) top = 0;

    // Title
    canvas->setTextSize(8);
    canvas->getTextBounds(title, 0, 0, &x1, &y1, &titleW, &titleH);
    int titleX = ((int)width - (int)titleW) / 2;
    int titleY = top;
    canvas->setCursor(titleX - x1, titleY - y1);
    canvas->print(title);

    // Status
    canvas->setTextSize(4);
    canvas->getTextBounds(status, 0, 0, &x1, &y1, &statusW, &statusH);
    int statusX = ((int)width - (int)statusW) / 2;
    int statusY = top + (int)titleH + gap;
    canvas->setCursor(statusX - x1, statusY - y1);
    canvas->print(status);

    if (progress >= 0) {
        const int barY = statusY + (int)statusH + gap;
        const int barW = (int)width - 120;
        int barX = (int)(width - barW) / 2;
        if (barX < 12) barX = 12;

        canvas->drawRect(barX, barY, barW, barHeight, 0);
        const int fillW = (barW - 2) * progress / 100;
        if (fillW > 0) {
            canvas->fillRect(barX + 1, barY + 1, fillW, barHeight - 2, 0);
        }
    }
}

void EInkUi::convertToG4() {
    if (!canvas || !g4Buffer) return;

    const uint8_t* mono = canvas->data();
    const uint32_t totalPixels = (uint32_t)width * (uint32_t)height;

    if (DISPLAY_ROTATION == 2) {
        memset(g4Buffer, 0xFF, g4BufferBytes);
        for (uint32_t srcIndex = 0; srcIndex < totalPixels; srcIndex++) {
            const uint32_t byteIndex = srcIndex >> 3;
            const uint8_t bitMask = (uint8_t)(0x80 >> (srcIndex & 0x07));
            const bool white = (mono[byteIndex] & bitMask) != 0;
            if (white) continue;

            const uint32_t dstIndex = totalPixels - 1 - srcIndex;
            const uint32_t dstByte = dstIndex >> 1;
            const uint8_t nibbleMask = (dstIndex & 1U) ? 0x0F : 0xF0;
            g4Buffer[dstByte] &= (uint8_t)(~nibbleMask);
        }
        return;
    }

    uint32_t srcIndex = 0;
    uint32_t dstIndex = 0;

    for (uint32_t i = 0; i < totalPixels; i += 2) {
        const uint32_t byteIndex1 = srcIndex >> 3;
        const uint8_t bitMask1 = (uint8_t)(0x80 >> (srcIndex & 0x07));
        const bool white1 = (mono[byteIndex1] & bitMask1) != 0;
        const uint8_t level1 = white1 ? 0x0F : 0x00;
        srcIndex++;

        uint8_t level2 = 0x0F;
        if (srcIndex < totalPixels) {
            const uint32_t byteIndex2 = srcIndex >> 3;
            const uint8_t bitMask2 = (uint8_t)(0x80 >> (srcIndex & 0x07));
            const bool white2 = (mono[byteIndex2] & bitMask2) != 0;
            level2 = white2 ? 0x0F : 0x00;
            srcIndex++;
        }

        g4Buffer[dstIndex++] = (uint8_t)((level1 << 4) | (level2 & 0x0F));
    }
}
