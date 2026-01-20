/*
 * Touch Manager
 * 
 * Manages touch controller lifecycle (no LVGL integration).
 */

#ifndef TOUCH_MANAGER_H
#define TOUCH_MANAGER_H

#include "board_config.h"

#if HAS_TOUCH

#include <Arduino.h>
#include "touch_driver.h"

class TouchManager {
private:
    TouchDriver* driver;
    uint32_t suppressUntilMs;
    bool forceReleased;
    
public:
    TouchManager();
    ~TouchManager();
    
    // Initialize touch hardware
    void init();

    // Periodic maintenance (optional)
    void loop();
    
    // Get touch state (for debugging)
    bool isTouched();
    bool getTouch(uint16_t* x, uint16_t* y);
};

// C-style interface for app.ino
void touch_manager_init();
void touch_manager_loop();
bool touch_manager_is_touched();

// Temporarily suppress touch input (forces released).
void touch_manager_suppress_lvgl_input(uint32_t duration_ms);

// Force touch to always report RELEASED while active.
void touch_manager_set_lvgl_force_released(bool force_released);

#endif // HAS_TOUCH

#endif // TOUCH_MANAGER_H
