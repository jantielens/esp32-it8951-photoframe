/*
 * Direct Image Screen Implementation
 * 
 * Direct image session for strip-by-strip image display.
 * Images are decoded and written directly to LCD hardware.
 */

#include "board_config.h"

#if HAS_IMAGE_API

#include "direct_image_screen.h"
#include "../display_manager.h"
#include "../log_manager.h"
#include <Arduino.h>

DirectImageScreen::DirectImageScreen(DisplayManager* mgr) 
    : manager(mgr), session_active(false), visible(false) {
    // Constructor - member variables initialized in initializer list
}

DirectImageScreen::~DirectImageScreen() {
    destroy();
}

void DirectImageScreen::create() {
    LOGI("DIRIMG", "Create start");

    // Set display driver for strip decoder
    if (manager) {
        decoder.setDisplayDriver(manager->getDriver());
    }
    
    LOGI("DIRIMG", "Create complete");
}

void DirectImageScreen::destroy() {
    LOGI("DIRIMG", "Destroy start");
    
    // End any active strip session
    if (session_active) {
        end_strip_session();
    }
    
    LOGI("DIRIMG", "Destroy complete");
}

void DirectImageScreen::update() {
    // Check timeout if visible
    if (visible && is_timeout_expired()) {
        LOGI("DIRIMG", "Timeout expired, returning to previous screen");

        // Clean up our state before returning to splash.
        if (session_active) {
            end_strip_session();
        }
        visible = false;
        display_start_time = 0;

        // Return to splash
        if (manager) {
            manager->showSplash();
            manager->renderNow();
        }
    }
}

void DirectImageScreen::show() {
    create();
    visible = true;

    // Start the timeout when the session is shown.
    // Using an uploader-provided start time (captured earlier during HTTP upload)
    // can cause rapid thrashing if the update loop is delayed and the timeout elapses
    // before the session becomes visible.
    display_start_time = millis();
    
    LOGI("DIRIMG", "Show (timeout: %lums)", display_timeout_ms);
}

void DirectImageScreen::hide() {
    visible = false;
    
    // End any active strip session
    if (session_active) {
        end_strip_session();
    }
    
    // Reset timeout
    display_start_time = 0;
}

void DirectImageScreen::begin_strip_session(int width, int height) {
    LOGI("DIRIMG", "Strip session start");
    LOGI("DIRIMG", "Image: %dx%d", width, height);

    // Each new upload should extend/restart the display timeout.
    // This is especially important when a new upload starts while we're already
    // on DirectImageScreen (show() won't be called again in that case).
    display_start_time = millis();

    // Ensure the strip decoder has a display driver even if the caller starts
    // decoding before this screen has been shown/created.
    if (manager) {
        decoder.setDisplayDriver(manager->getDriver());
    }
    
    // Use the display driver's coordinate space (what setAddrWindow expects).
    // This is the fast-path contract for direct-image uploads.
    int lcd_width = DISPLAY_WIDTH;
    int lcd_height = DISPLAY_HEIGHT;
    if (manager && manager->getDriver()) {
        lcd_width = manager->getDriver()->width();
        lcd_height = manager->getDriver()->height();
    }
    
    // Initialize decoder
    decoder.begin(width, height, lcd_width, lcd_height);
    session_active = true;
    
    LOGI("DIRIMG", "Strip session ready");
}

bool DirectImageScreen::decode_strip(const uint8_t* jpeg_data, size_t jpeg_size, int strip_index, bool output_bgr565) {
    if (!session_active) {
        LOGE("DIRIMG", "No active strip session");
        return false;
    }
    
    // Decode strip and write directly to LCD
    bool success = decoder.decode_strip(jpeg_data, jpeg_size, strip_index, output_bgr565);
    
    if (!success) {
        LOGE("DIRIMG", "Strip %d decode failed", strip_index);
    }
    
    return success;
}

void DirectImageScreen::end_strip_session() {
    if (!session_active) return;
    
    LOGI("DIRIMG", "End strip session");
    
    decoder.end();
    session_active = false;
}

void DirectImageScreen::set_timeout(unsigned long timeout_ms) {
    display_timeout_ms = timeout_ms;
    LOGI("DIRIMG", "Timeout set to %lu ms", timeout_ms);
}

void DirectImageScreen::set_start_time(unsigned long start_time) {
    display_start_time = start_time;
    LOGI("DIRIMG", "Start time set to %lu", start_time);
}

bool DirectImageScreen::is_timeout_expired() {
    // 0 means display forever
    if (display_timeout_ms == 0) {
        return false;
    }

    const unsigned long now = millis();

    // If start time is unset, or is slightly ahead of 'now' (possible when a
    // different task updates start_time and the update loop checks it before the
    // tick counter advances), rebase instead of underflowing.
    if (display_start_time == 0 || display_start_time > now) {
        display_start_time = now;
        return false;
    }

    // Check if timeout has elapsed
    unsigned long elapsed = now - display_start_time;
    return elapsed >= display_timeout_ms;
}

#endif // HAS_IMAGE_API
