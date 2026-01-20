#include "drivers/it8951_lvgl_driver.h"

#include "../board_config.h"
#include "../it8951_renderer.h"
#include "../log_manager.h"

#include <esp_heap_caps.h>
#include <string.h>

IT8951_LVGL_Driver::IT8951_LVGL_Driver()
        : dispWidth(0), dispHeight(0), logicalWidth(0), logicalHeight(0), rotation(0),
            g4FrameBuffer(nullptr), g4BufferBytes(0), dirtyValid(false),
            dirtyX1(0), dirtyY1(0), dirtyX2(0), dirtyY2(0) {}

IT8951_LVGL_Driver::~IT8951_LVGL_Driver() {
    if (g4FrameBuffer) {
        heap_caps_free(g4FrameBuffer);
        g4FrameBuffer = nullptr;
    }
}

void IT8951_LVGL_Driver::init() {
    if (!it8951_renderer_init()) {
        LOGE("EINK", "Renderer init failed");
        return;
    }

    dispWidth = DISPLAY_WIDTH;
    dispHeight = DISPLAY_HEIGHT;
    rotation = DISPLAY_ROTATION;
    logicalWidth = dispWidth;
    logicalHeight = dispHeight;
    g4BufferBytes = (size_t)dispWidth * (size_t)dispHeight / 2;

    g4FrameBuffer = static_cast<uint8_t*>(heap_caps_malloc(g4BufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g4FrameBuffer) {
        g4FrameBuffer = static_cast<uint8_t*>(heap_caps_malloc(g4BufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    if (!g4FrameBuffer) {
        LOGE("EINK", "Failed to allocate G4 framebuffer (%u bytes)", (unsigned)g4BufferBytes);
        return;
    }

    // Initialize to white (0xF per nibble).
    memset(g4FrameBuffer, 0xFF, g4BufferBytes);
    LOGI("EINK", "G4 framebuffer allocated: %u bytes", (unsigned)g4BufferBytes);

    dirtyValid = false;
}

int IT8951_LVGL_Driver::width() {
    return dispWidth;
}

int IT8951_LVGL_Driver::height() {
    return dispHeight;
}

DisplayDriver::RenderMode IT8951_LVGL_Driver::renderMode() const {
    return RenderMode::Buffered;
}

uint8_t IT8951_LVGL_Driver::rgb565_to_gray4(uint16_t rgb) const {
    const uint8_t r5 = (rgb >> 11) & 0x1F;
    const uint8_t g6 = (rgb >> 5) & 0x3F;
    const uint8_t b5 = rgb & 0x1F;

    const uint8_t r8 = (r5 << 3) | (r5 >> 2);
    const uint8_t g8 = (g6 << 2) | (g6 >> 4);
    const uint8_t b8 = (b5 << 3) | (b5 >> 2);

    const uint8_t gray = (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);
    return (uint8_t)(gray >> 4);
}

void IT8951_LVGL_Driver::write_gray4_pixel(uint16_t x, uint16_t y, uint8_t level) {
    const uint32_t idx = (uint32_t)y * (uint32_t)dispWidth + x;
    const uint32_t byteIndex = idx >> 1;

    if ((idx & 1U) == 0) {
        g4FrameBuffer[byteIndex] = (uint8_t)((g4FrameBuffer[byteIndex] & 0x0F) | (level << 4));
    } else {
        g4FrameBuffer[byteIndex] = (uint8_t)((g4FrameBuffer[byteIndex] & 0xF0) | (level & 0x0F));
    }
}

void IT8951_LVGL_Driver::expand_dirty_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    if (!dirtyValid) {
        dirtyValid = true;
        dirtyX1 = x1;
        dirtyY1 = y1;
        dirtyX2 = x2;
        dirtyY2 = y2;
        return;
    }

    if (x1 < dirtyX1) dirtyX1 = x1;
    if (y1 < dirtyY1) dirtyY1 = y1;
    if (x2 > dirtyX2) dirtyX2 = x2;
    if (y2 > dirtyY2) dirtyY2 = y2;
}

void IT8951_LVGL_Driver::flushArea(const lv_area_t* area, lv_color_t* color_p) {
    if (!g4FrameBuffer || !area || !color_p) return;

    const uint16_t area_w = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t area_h = (uint16_t)(area->y2 - area->y1 + 1);

    uint32_t src_index = 0;
    // Track dirty rectangle in destination coordinates (panel space).
    uint16_t rect_x1 = 0;
    uint16_t rect_y1 = 0;
    uint16_t rect_x2 = 0;
    uint16_t rect_y2 = 0;
    bool rect_valid = true;

    switch (rotation) {
        case 1: // 90° clockwise
            rect_x1 = (uint16_t)(dispWidth - 1 - area->y2);
            rect_x2 = (uint16_t)(dispWidth - 1 - area->y1);
            rect_y1 = (uint16_t)area->x1;
            rect_y2 = (uint16_t)area->x2;
            break;
        case 2: // 180°
            rect_x1 = (uint16_t)(dispWidth - 1 - area->x2);
            rect_x2 = (uint16_t)(dispWidth - 1 - area->x1);
            rect_y1 = (uint16_t)(dispHeight - 1 - area->y2);
            rect_y2 = (uint16_t)(dispHeight - 1 - area->y1);
            break;
        case 3: // 270° clockwise
            rect_x1 = (uint16_t)area->y1;
            rect_x2 = (uint16_t)area->y2;
            rect_y1 = (uint16_t)(dispHeight - 1 - area->x2);
            rect_y2 = (uint16_t)(dispHeight - 1 - area->x1);
            break;
        default:
            rect_x1 = (uint16_t)area->x1;
            rect_y1 = (uint16_t)area->y1;
            rect_x2 = (uint16_t)area->x2;
            rect_y2 = (uint16_t)area->y2;
            break;
    }

    if (rect_x1 >= dispWidth || rect_y1 >= dispHeight) {
        rect_valid = false;
    } else {
        if (rect_x2 >= dispWidth) rect_x2 = (uint16_t)(dispWidth - 1);
        if (rect_y2 >= dispHeight) rect_y2 = (uint16_t)(dispHeight - 1);
    }

    if (rect_valid) {
        expand_dirty_rect(rect_x1, rect_y1, rect_x2, rect_y2);
    }

    for (uint16_t y = 0; y < area_h; y++) {
        const uint16_t src_y = (uint16_t)(area->y1 + y);
        if (src_y >= logicalHeight) {
            src_index += area_w;
            continue;
        }
        for (uint16_t x = 0; x < area_w; x++) {
            const uint16_t src_x = (uint16_t)(area->x1 + x);
            if (src_x >= logicalWidth) {
                src_index++;
                continue;
            }
            uint16_t dst_x = src_x;
            uint16_t dst_y = src_y;

            switch (rotation) {
                case 1: // 90° clockwise
                    dst_x = (uint16_t)(dispWidth - 1 - src_y);
                    dst_y = src_x;
                    break;
                case 2: // 180°
                    dst_x = (uint16_t)(dispWidth - 1 - src_x);
                    dst_y = (uint16_t)(dispHeight - 1 - src_y);
                    break;
                case 3: // 270° clockwise
                    dst_x = src_y;
                    dst_y = (uint16_t)(dispHeight - 1 - src_x);
                    break;
                default:
                    break;
            }

            if (dst_x < dispWidth && dst_y < dispHeight) {
                const uint16_t rgb = color_p[src_index].full;
                const uint8_t level = rgb565_to_gray4(rgb);
                write_gray4_pixel(dst_x, dst_y, level);
            }
            src_index++;
        }
    }
}

void IT8951_LVGL_Driver::present() {
    present(false);
}

void IT8951_LVGL_Driver::present(bool fullRefresh) {
    if (!g4FrameBuffer) return;
    if (it8951_renderer_is_busy()) return;

    if (fullRefresh) {
        const unsigned long start_ms = millis();
        if (!it8951_render_g4_buffer(g4FrameBuffer, dispWidth, dispHeight)) {
            LOGE("EINK", "Present full failed");
            return;
        }
        LOG_DURATION("EINK", "Present full", start_ms);
        dirtyValid = false;
        return;
    }

    if (!dirtyValid) return;

    uint16_t x = dirtyX1;
    uint16_t y = dirtyY1;
    uint16_t w = (uint16_t)(dirtyX2 - dirtyX1 + 1);
    uint16_t h = (uint16_t)(dirtyY2 - dirtyY1 + 1);

    // Clamp to panel bounds.
    if (x >= dispWidth || y >= dispHeight) {
        dirtyValid = false;
        return;
    }
    if (x + w > dispWidth) w = dispWidth - x;
    if (y + h > dispHeight) h = dispHeight - y;

    dirtyValid = false;

    const unsigned long start_ms = millis();
    if (!it8951_render_g4_buffer_region(g4FrameBuffer, dispWidth, dispHeight, x, y, w, h)) {
        LOGE("EINK", "Present failed");
        return;
    }
    LOG_DURATION("EINK", "Present", start_ms);
}

void IT8951_LVGL_Driver::configureLVGL(lv_disp_drv_t* drv) {
    if (!drv) return;
    drv->full_refresh = 0;

    // For 90°/270° rotations, swap LVGL logical resolution to match mapping.
    if (rotation == 1 || rotation == 3) {
        drv->hor_res = DISPLAY_HEIGHT;
        drv->ver_res = DISPLAY_WIDTH;
        logicalWidth = DISPLAY_HEIGHT;
        logicalHeight = DISPLAY_WIDTH;
    } else {
        logicalWidth = DISPLAY_WIDTH;
        logicalHeight = DISPLAY_HEIGHT;
    }
}

uint32_t IT8951_LVGL_Driver::minPresentIntervalMs() const {
    return EINK_MIN_PRESENT_INTERVAL_MS;
}
