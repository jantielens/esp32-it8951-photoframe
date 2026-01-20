#include "board_config.h"

#if HAS_TOUCH

#include "touch_manager.h"
#include "log_manager.h"

// Touch init can run while other display work is active.

// Include selected touch driver header.
// Driver implementations are compiled via src/app/touch_drivers.cpp.
#if TOUCH_DRIVER == TOUCH_DRIVER_XPT2046
#include "drivers/xpt2046_driver.h"
#elif TOUCH_DRIVER == TOUCH_DRIVER_AXS15231B
#include "drivers/axs15231b_touch_driver.h"
#elif TOUCH_DRIVER == TOUCH_DRIVER_CST816S_ESP_PANEL
#include "drivers/esp_panel_cst816s_touch_driver.h"
#endif

// Global instance
TouchManager* touchManager = nullptr;

static uint32_t g_touch_suppress_until_ms = 0;
static bool g_touch_force_released = false;

TouchManager::TouchManager()
    : driver(nullptr), suppressUntilMs(0), forceReleased(false) {
    // Driver will be instantiated in init() after display is ready
}

TouchManager::~TouchManager() {
    if (driver) {
        delete driver;
        driver = nullptr;
    }
}

void TouchManager::init() {
    LOGI("Touch", "Manager init start");

    suppressUntilMs = g_touch_suppress_until_ms;
    forceReleased = g_touch_force_released;
    
    // Create standalone touch driver (no dependency on display)
    #if TOUCH_DRIVER == TOUCH_DRIVER_XPT2046
    driver = new XPT2046_Driver(TOUCH_CS, TOUCH_IRQ);
    #elif TOUCH_DRIVER == TOUCH_DRIVER_AXS15231B
    driver = new AXS15231B_TouchDriver();
    #elif TOUCH_DRIVER == TOUCH_DRIVER_CST816S_ESP_PANEL
    driver = new ESPPanel_CST816S_TouchDriver();
    #else
    #error "No touch driver selected or unknown driver type"
    #endif
    
    // Initialize hardware
    driver->init();
    
    // Set calibration if defined
    #if defined(TOUCH_CAL_X_MIN) && defined(TOUCH_CAL_X_MAX) && defined(TOUCH_CAL_Y_MIN) && defined(TOUCH_CAL_Y_MAX)
    driver->setCalibration(TOUCH_CAL_X_MIN, TOUCH_CAL_X_MAX, TOUCH_CAL_Y_MIN, TOUCH_CAL_Y_MAX);
    #endif
    
    // Set rotation to match display
    #ifdef DISPLAY_ROTATION
    driver->setRotation(DISPLAY_ROTATION);
    LOGI("Touch", "Rotation: %d", DISPLAY_ROTATION);
    #endif

    LOGI("Touch", "Manager init complete");
}

void TouchManager::loop() {
    // No-op for now
}

bool TouchManager::isTouched() {
    const uint32_t now = millis();
    if (forceReleased || ((int32_t)(suppressUntilMs - now) > 0)) {
        return false;
    }
    return driver->isTouched();
}

bool TouchManager::getTouch(uint16_t* x, uint16_t* y) {
    const uint32_t now = millis();
    if (forceReleased || ((int32_t)(suppressUntilMs - now) > 0)) {
        return false;
    }
    return driver->getTouch(x, y);
}

// C-style interface for app.ino
void touch_manager_init() {
    if (!touchManager) {
        touchManager = new TouchManager();
    }
    touchManager->init();
}

void touch_manager_loop() {
    if (!touchManager) return;
    touchManager->loop();
}

bool touch_manager_is_touched() {
    if (!touchManager) return false;
    return touchManager->isTouched();
}

void touch_manager_suppress_lvgl_input(uint32_t duration_ms) {
    const uint32_t now = millis();
    const uint32_t until = now + duration_ms;
    // Extend suppression window if already active.
    if ((int32_t)(g_touch_suppress_until_ms - until) < 0) {
        g_touch_suppress_until_ms = until;
    }
    if (touchManager) {
        touchManager->suppressUntilMs = g_touch_suppress_until_ms;
    }
}

void touch_manager_set_lvgl_force_released(bool force_released) {
    g_touch_force_released = force_released;
    if (touchManager) {
        touchManager->forceReleased = force_released;
    }
}

#endif // HAS_TOUCH
