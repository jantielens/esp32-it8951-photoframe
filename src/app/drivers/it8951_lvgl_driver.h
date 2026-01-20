#pragma once

#include "../display_driver.h"

class IT8951_LVGL_Driver : public DisplayDriver {
public:
    IT8951_LVGL_Driver();
    ~IT8951_LVGL_Driver() override;

    void init() override;
    int width() override;
    int height() override;

    RenderMode renderMode() const override;
    void flushArea(const lv_area_t* area, lv_color_t* color_p) override;
    void present() override;
    void present(bool fullRefresh) override;
    void configureLVGL(lv_disp_drv_t* drv) override;
    uint32_t minPresentIntervalMs() const override;

private:
    uint16_t dispWidth;
    uint16_t dispHeight;
    uint16_t logicalWidth;
    uint16_t logicalHeight;
    uint8_t rotation;
    uint8_t* g4FrameBuffer;
    size_t g4BufferBytes;
    bool dirtyValid;
    uint16_t dirtyX1;
    uint16_t dirtyY1;
    uint16_t dirtyX2;
    uint16_t dirtyY2;

    uint8_t rgb565_to_gray4(uint16_t rgb) const;
    void write_gray4_pixel(uint16_t x, uint16_t y, uint8_t level);
    void expand_dirty_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
};
