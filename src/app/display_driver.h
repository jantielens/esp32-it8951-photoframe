/*
 * Display Driver Interface
 * 
 * Hardware abstraction layer for e-ink display drivers.
 * 
 * IMPLEMENTATION GUIDE FOR NEW DRIVERS:
 * =====================================
 * 
 * 1. Create driver class implementing this interface:
 *    - drivers/your_driver.h (interface)
 *    - drivers/your_driver.cpp (implementation)
 *
 * 2. Register implementation include in src/app/display_drivers.cpp
 */

#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <Arduino.h>

// ============================================================================
// Display Driver Interface
// ============================================================================
// Pure virtual interface for display hardware abstraction.
// Minimal set of methods required for e-ink UI rendering.

class DisplayDriver {
public:
    virtual ~DisplayDriver() = default;

    // Hardware initialization
    virtual void init() = 0;
    
    // Active coordinate space dimensions.
    virtual int width() = 0;
    virtual int height() = 0;

    // Busy guard for long-running transfers.
    virtual bool isBusy() const = 0;

    // Present a full-screen 4bpp (packed) buffer.
    virtual bool presentG4Full(const uint8_t* g4, bool fullRefresh) = 0;

    // Present a region 4bpp (packed) buffer.
    virtual bool presentG4Region(const uint8_t* g4, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h, bool fullRefresh) = 0;

    // Optional direct RGB565 write path (used by JPEG strip decoder on color panels).
    virtual void startWrite() {}
    virtual void endWrite() {}
    virtual void setAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) {
        (void)x; (void)y; (void)w; (void)h;
    }
    virtual void pushColors(uint16_t* data, uint32_t len, bool swap_bytes) {
        (void)data; (void)len; (void)swap_bytes;
    }

    // Optional backlight controls (mostly for TFT panels).
    virtual void setBacklight(bool on) { (void)on; }
    virtual void setBacklightBrightness(uint8_t brightness) { (void)brightness; }
    virtual uint8_t getBacklightBrightness() { return 0; }
    virtual bool hasBacklightControl() { return false; }

    // Minimum time between present() calls. E-ink panels are slow; throttle updates.
    virtual uint32_t minPresentIntervalMs() const { return 0; }
};

#endif // DISPLAY_DRIVER_H
