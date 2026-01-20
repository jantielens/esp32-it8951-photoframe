#include "drivers/it8951_display_driver.h"

#include "../board_config.h"
#include "../it8951_renderer.h"
#include "../log_manager.h"

void IT8951_Display_Driver::init() {
    if (!it8951_renderer_init()) {
        LOGE("EINK", "Renderer init failed");
    }
}

int IT8951_Display_Driver::width() {
    return DISPLAY_WIDTH;
}

int IT8951_Display_Driver::height() {
    return DISPLAY_HEIGHT;
}

bool IT8951_Display_Driver::isBusy() const {
    return it8951_renderer_is_busy();
}

bool IT8951_Display_Driver::presentG4Full(const uint8_t* g4, bool fullRefresh) {
    return it8951_render_g4_buffer_ex(g4, DISPLAY_WIDTH, DISPLAY_HEIGHT, fullRefresh);
}

bool IT8951_Display_Driver::presentG4Region(const uint8_t* g4, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h, bool fullRefresh) {
    return it8951_render_g4_region(g4, x, y, w, h, fullRefresh);
}

uint32_t IT8951_Display_Driver::minPresentIntervalMs() const {
    return EINK_MIN_PRESENT_INTERVAL_MS;
}
