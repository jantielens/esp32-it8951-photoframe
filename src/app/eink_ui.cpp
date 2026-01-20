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
            g4RegionBuffer(nullptr), g4RegionBytes(0), width(0), height(0),
            currentBounds({0, 0, 0, 0, false}), lastBounds({0, 0, 0, 0, false}),
            lastRenderPartial(false), progress(-1) {
    title[0] = '\0';
    status[0] = '\0';
}

EInkUi::~EInkUi() {
    if (g4Buffer) {
        heap_caps_free(g4Buffer);
        g4Buffer = nullptr;
    }
    if (g4RegionBuffer) {
        heap_caps_free(g4RegionBuffer);
        g4RegionBuffer = nullptr;
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
    return render(fullRefresh, true);
}

EInkUi::Rect EInkUi::unionRect(const Rect& a, const Rect& b) {
    if (!a.valid) return b;
    if (!b.valid) return a;

    const uint16_t x1 = min(a.x, b.x);
    const uint16_t y1 = min(a.y, b.y);
    const uint16_t x2 = max((uint16_t)(a.x + a.w), (uint16_t)(b.x + b.w));
    const uint16_t y2 = max((uint16_t)(a.y + a.h), (uint16_t)(b.y + b.h));

    return {x1, y1, (uint16_t)(x2 - x1), (uint16_t)(y2 - y1), true};
}

EInkUi::Rect EInkUi::clampRect(const Rect& r, uint16_t maxW, uint16_t maxH) {
    if (!r.valid) return r;
    Rect out = r;
    if (out.x >= maxW || out.y >= maxH) return {0, 0, 0, 0, false};
    if (out.x + out.w > maxW) out.w = maxW - out.x;
    if (out.y + out.h > maxH) out.h = maxH - out.y;
    if (out.w == 0 || out.h == 0) return {0, 0, 0, 0, false};
    return out;
}

EInkUi::Rect EInkUi::alignRectEven(const Rect& r, uint16_t maxW, uint16_t maxH) {
    if (!r.valid) return r;
    Rect out = r;
    if (out.x & 1U) {
        if (out.x > 0) out.x -= 1;
        if (out.x + out.w < maxW) out.w += 1;
    }
    if (out.w & 1U) {
        if (out.x + out.w < maxW) {
            out.w += 1;
        } else if (out.w > 1) {
            out.w -= 1;
        }
    }
    return clampRect(out, maxW, maxH);
}

bool EInkUi::ensureRegionBuffer(uint16_t w, uint16_t h) {
    const size_t needed = (size_t)w * (size_t)h / 2;
    if (g4RegionBuffer && g4RegionBytes >= needed) return true;

    if (g4RegionBuffer) {
        heap_caps_free(g4RegionBuffer);
        g4RegionBuffer = nullptr;
        g4RegionBytes = 0;
    }

    g4RegionBuffer = static_cast<uint8_t*>(heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g4RegionBuffer) {
        g4RegionBuffer = static_cast<uint8_t*>(heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!g4RegionBuffer) {
        LOGE("UI", "G4 region alloc failed (%u bytes)", (unsigned)needed);
        return false;
    }
    g4RegionBytes = needed;
    return true;
}

bool EInkUi::render(bool fullRefresh, bool allowPartial) {
    if (!driver || !canvas || !g4Buffer) return false;

    redraw();
    convertToG4();

    lastRenderPartial = false;

    if (fullRefresh || !allowPartial) {
        lastBounds = currentBounds;
        return driver->presentG4Full(g4Buffer, fullRefresh);
    }

    Rect dirty = unionRect(lastBounds, currentBounds);
    dirty = clampRect(dirty, width, height);
    if (!dirty.valid || (dirty.w == width && dirty.h == height)) {
        lastBounds = currentBounds;
        return driver->presentG4Full(g4Buffer, false);
    }

    // Apply 180° rotation to dirty bounds if needed.
    Rect rotated = dirty;
    if (DISPLAY_ROTATION == 2) {
        rotated.x = (uint16_t)(width - (dirty.x + dirty.w));
        rotated.y = (uint16_t)(height - (dirty.y + dirty.h));
    }

    rotated = alignRectEven(rotated, width, height);
    if (!rotated.valid) {
        lastBounds = currentBounds;
        return driver->presentG4Full(g4Buffer, false);
    }

    if (!ensureRegionBuffer(rotated.w, rotated.h)) {
        lastBounds = currentBounds;
        return driver->presentG4Full(g4Buffer, false);
    }

    const uint16_t packed_width = width / 2;
    const uint16_t region_packed = rotated.w / 2;

    for (uint16_t row = 0; row < rotated.h; row++) {
        const uint32_t src_row = (uint32_t)(rotated.y + row);
        const uint32_t src_offset = src_row * packed_width + (rotated.x / 2);
        const uint32_t dst_offset = (uint32_t)row * region_packed;
        memcpy(&g4RegionBuffer[dst_offset], &g4Buffer[src_offset], region_packed);
    }

    const bool ok = driver->presentG4Region(g4RegionBuffer, rotated.x, rotated.y, rotated.w, rotated.h, false);
    lastRenderPartial = ok;
    lastBounds = currentBounds;
    return ok;
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

    Rect bounds = {0, 0, 0, 0, false};
    const int padding = 4;

    auto add_bounds = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        Rect r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, true};
        bounds = unionRect(bounds, r);
    };

    // Title
    canvas->setTextSize(8);
    canvas->getTextBounds(title, 0, 0, &x1, &y1, &titleW, &titleH);
    int titleX = ((int)width - (int)titleW) / 2;
    int titleY = top;
    canvas->setCursor(titleX - x1, titleY - y1);
    canvas->print(title);
    add_bounds(titleX, titleY, (int)titleW, (int)titleH);

    // Status
    canvas->setTextSize(4);
    canvas->getTextBounds(status, 0, 0, &x1, &y1, &statusW, &statusH);
    int statusX = ((int)width - (int)statusW) / 2;
    int statusY = top + (int)titleH + gap;
    canvas->setCursor(statusX - x1, statusY - y1);
    canvas->print(status);
    add_bounds(statusX, statusY, (int)statusW, (int)statusH);

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
        add_bounds(barX, barY, barW, barHeight);
    }

    if (bounds.valid) {
        int x = (int)bounds.x - padding;
        int y = (int)bounds.y - padding;
        int w = (int)bounds.w + padding * 2;
        int h = (int)bounds.h + padding * 2;

        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int)width) w = (int)width - x;
        if (y + h > (int)height) h = (int)height - y;

        currentBounds = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, (w > 0 && h > 0)};
    } else {
        currentBounds = {0, 0, 0, 0, false};
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
