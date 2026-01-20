/*
 * Direct Image Screen
 * 
 * Direct image session for strip-by-strip image display.
 * Strips are decoded and written directly to LCD hardware.
 * 
 * Default timeout: 10 seconds (configurable via set_timeout)
 */

#pragma once

#include "board_config.h"

#if HAS_IMAGE_API

#include "../strip_decoder.h"

// Forward declaration
class DisplayManager;

class DirectImageScreen {
public:
    DirectImageScreen(DisplayManager* mgr);
    ~DirectImageScreen();
    
    void create();
    void destroy();
    void update();
    void show();
    void hide();
    
    // Strip upload session management
    // width: image width in pixels
    // height: image height in pixels
    void begin_strip_session(int width, int height);
    
    // Decode and display a single strip
    // Returns: true on success, false on failure
    bool decode_strip(const uint8_t* jpeg_data, size_t jpeg_size, int strip_index, bool output_bgr565 = true);
    
    // End strip upload session
    void end_strip_session();
    
    // Set display timeout in milliseconds (default: 10 seconds, 0 = forever)
    void set_timeout(unsigned long timeout_ms);
    
    // Set start time (for accurate timeout when upload completes)
    void set_start_time(unsigned long start_time);
    
    // Check if timeout expired
    bool is_timeout_expired();
    
    // Get strip decoder for progress tracking
    StripDecoder* get_decoder() { return &decoder; }
    
private:
    DisplayManager* manager;    // Display manager reference
    StripDecoder decoder;       // JPEG decoder
    
    // Timeout tracking
    unsigned long display_start_time = 0;
    unsigned long display_timeout_ms = 10000;  // Default 10 seconds
    
    // Session state
    bool session_active = false;
    bool visible = false;
};

#endif // HAS_IMAGE_API
