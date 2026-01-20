/*
 * Display Driver Interface
 * 
 * Hardware abstraction layer for e-ink display drivers.
 * LVGL flushes write into a framebuffer; present() pushes to the panel.
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
#include <lvgl.h>

// ============================================================================
// Display Driver Interface
// ============================================================================
// Pure virtual interface for display hardware abstraction.
// Minimal set of methods required for LVGL integration.

class DisplayDriver {
public:
    virtual ~DisplayDriver() = default;

    enum class RenderMode : uint8_t {
        Direct = 0,
        Buffered = 1,
    };
    
    // Hardware initialization
    virtual void init() = 0;
    
    // Active coordinate space dimensions for the LVGL framebuffer.
    virtual int width() = 0;
    virtual int height() = 0;

    // Declare whether the driver is Direct or Buffered.
    // Default: Buffered for e-ink (flush writes into a framebuffer).
    virtual RenderMode renderMode() const {
        return RenderMode::Buffered;
    }

    // LVGL flush callback handler: copy the area into the driver's framebuffer.
    virtual void flushArea(const lv_area_t* area, lv_color_t* color_p) = 0;

    // For buffered drivers, push the accumulated framebuffer to the panel.
    virtual void present() {}

    // Optional full-refresh present (fallbacks to present()).
    virtual void present(bool fullRefresh) { (void)fullRefresh; present(); }
    
    // LVGL configuration hook (override to customize LVGL driver settings)
    // Called during LVGL initialization to allow driver-specific configuration
    // such as software rotation, full refresh mode, etc.
    // Default implementation: no special configuration needed
    virtual void configureLVGL(lv_disp_drv_t* drv) {}

    // Minimum time between present() calls. E-ink panels are slow; throttle updates.
    virtual uint32_t minPresentIntervalMs() const { return 0; }
};

#endif // DISPLAY_DRIVER_H
