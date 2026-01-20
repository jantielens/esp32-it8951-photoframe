#pragma once

#include "../display_driver.h"

class IT8951_Display_Driver : public DisplayDriver {
public:
    IT8951_Display_Driver() = default;
    ~IT8951_Display_Driver() override = default;

    void init() override;
    int width() override;
    int height() override;
    bool isBusy() const override;

    bool presentG4Full(const uint8_t* g4, bool fullRefresh) override;
    bool presentG4Region(const uint8_t* g4, uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h, bool fullRefresh) override;

    uint32_t minPresentIntervalMs() const override;
};
