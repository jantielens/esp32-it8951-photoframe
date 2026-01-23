#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ============================================================================
// Board Configuration - Two-Phase Include Pattern
// ============================================================================
// This file provides default configuration for all boards using a two-phase
// include pattern:
//
// Phase 1: Load board-specific overrides first (if they exist)
// Phase 2: Define defaults using #ifndef guards (only if not already defined)
//
// To customize for a specific board, create: src/boards/[board-name]/board_overrides.h
// The build system will automatically detect and include board-specific overrides.
//
// Example board-specific override:
//   src/boards/esp32c3-waveshare-169-st7789v2/board_overrides.h

// ============================================================================
// Phase 1: Load Board-Specific Overrides
// ============================================================================
// build.sh defines BOARD_HAS_OVERRIDE when a board override directory exists
// and adds that directory to the include path. Board-specific settings are
// loaded first so they can override the defaults below.

#ifdef BOARD_HAS_OVERRIDE
#include "board_overrides.h"
#endif

// ============================================================================
// Project Branding
// ============================================================================
// Human-friendly project name used in the web UI and device name (can be set by build system).
#ifndef PROJECT_DISPLAY_NAME
#define PROJECT_DISPLAY_NAME "ESP32 Device"
#endif

// ============================================================================
// Phase 2: Default Hardware Capabilities
// ============================================================================
// These defaults are only applied if not already defined by board overrides.

// Enable built-in status LED support.
#ifndef HAS_BUILTIN_LED
#define HAS_BUILTIN_LED false
#endif

// Enable MQTT and Home Assistant integration.
#ifndef HAS_MQTT
#define HAS_MQTT true
#endif

// GPIO for the built-in LED (only used when HAS_BUILTIN_LED is true).
#ifndef LED_PIN
#define LED_PIN 2  // Common GPIO for ESP32 boards
#endif

// LED polarity: true if HIGH turns the LED on.
#ifndef LED_ACTIVE_HIGH
#define LED_ACTIVE_HIGH true  // true = HIGH turns LED on, false = LOW turns LED on
#endif

// ============================================================================
// Default WiFi Configuration
// ============================================================================

// Maximum WiFi connection attempts at boot before falling back.
#ifndef WIFI_MAX_ATTEMPTS
#define WIFI_MAX_ATTEMPTS 3
#endif

// ============================================================================
// SD Card (SPI)
// ============================================================================
// SPI clock used for SD.begin(..., frequency_hz).
// 80MHz is optimistic and some breakouts/cards will fail; override per-board.
#ifndef SD_SPI_FREQUENCY_HZ
#define SD_SPI_FREQUENCY_HZ 80000000
#endif

// If true, use the global `SPI` instance for SD instead of a separate SPI peripheral.
// Set this when SD shares the same SCK/MISO/MOSI pins as the display.
#ifndef SD_USE_ARDUINO_SPI
#define SD_USE_ARDUINO_SPI false
#endif

// ============================================================================
// Default Boot/Power Behavior
// ============================================================================
// Default for DeviceConfig.always_on when no NVS config exists yet.
// Useful for boards where boot-time button/EXT1 wake isn't wired.
#ifndef DEFAULT_ALWAYS_ON
#define DEFAULT_ALWAYS_ON false
#endif

// ============================================================================
// Optional: Touch Wake (deep sleep)
// ============================================================================
// Touch wake is disabled by default because touch pads vary per MCU/board and
// a misconfigured pad can cause immediate wake loops.
//
// Set this in a board override to a valid touch pad index (NOT a GPIO number).
// Example (ESP32-S2): 6 for TOUCH06.
#ifndef TOUCH_WAKE_PAD
#define TOUCH_WAKE_PAD -1
#endif

// ============================================================================
// Additional Default Configuration Settings
// ============================================================================
// Add new hardware features here using #ifndef guards to allow board-specific
// overrides.
//
// Usage Pattern in Application Code:
//   1. Define capabilities in board_overrides.h: #define HAS_BUTTON true
//   2. Use conditional compilation in app.ino:
//
//      #if HAS_BUTTON
//        pinMode(BUTTON_PIN, INPUT_PULLUP);
//        // Button-specific code only compiled when HAS_BUTTON is true
//      #endif
//
// Examples:
//
// Button:
// #ifndef HAS_BUTTON
// #define HAS_BUTTON false
// #endif
//
// #ifndef BUTTON_PIN
// #define BUTTON_PIN 0
// #endif
// Web Portal Health Widget
// ============================================================================
// How often the web UI polls /api/health.
#ifndef HEALTH_POLL_INTERVAL_MS
#define HEALTH_POLL_INTERVAL_MS 5000
#endif

// How much client-side history (sparklines) to keep.
#ifndef HEALTH_HISTORY_SECONDS
#define HEALTH_HISTORY_SECONDS 300
#endif

// ============================================================================
// Optional: Device-side Health History (/api/health/history)
// ============================================================================
// When enabled, firmware keeps a fixed-size ring buffer for sparklines so the
// portal can render history even when no client was connected.
// Default: enabled.
#ifndef HEALTH_HISTORY_ENABLED
#define HEALTH_HISTORY_ENABLED 1
#endif

// Sampling cadence for the device-side history (ms). Default aligns with UI poll.
#ifndef HEALTH_HISTORY_PERIOD_MS
#define HEALTH_HISTORY_PERIOD_MS 5000
#endif

#if HEALTH_HISTORY_ENABLED
// Derived number of samples.
#ifndef HEALTH_HISTORY_SAMPLES
#define HEALTH_HISTORY_SAMPLES ((HEALTH_HISTORY_SECONDS * 1000) / HEALTH_HISTORY_PERIOD_MS)
#endif

// Guardrails (must compile in both C and C++ translation units).
#if (HEALTH_HISTORY_PERIOD_MS < 1000)
#error HEALTH_HISTORY_PERIOD_MS too small
#endif

#if (((HEALTH_HISTORY_SECONDS * 1000UL) % (HEALTH_HISTORY_PERIOD_MS)) != 0)
#error HEALTH_HISTORY_SECONDS must be divisible by HEALTH_HISTORY_PERIOD_MS
#endif

#if (HEALTH_HISTORY_SAMPLES < 10)
#error HEALTH_HISTORY_SAMPLES too small
#endif

#if (HEALTH_HISTORY_SAMPLES > 600)
#error HEALTH_HISTORY_SAMPLES too large
#endif
#endif

// ============================================================================
// Display Configuration
// ============================================================================
// Display driver selection (e-ink).
#define DISPLAY_DRIVER_IT8951 1

// Select the display HAL backend.
#ifndef DISPLAY_DRIVER
#define DISPLAY_DRIVER DISPLAY_DRIVER_IT8951
#endif

// Display dimensions.
#ifndef DISPLAY_WIDTH
	#error DISPLAY_WIDTH must be defined
#endif
#ifndef DISPLAY_HEIGHT
	#error DISPLAY_HEIGHT must be defined
#endif
#ifndef DISPLAY_ROTATION
	#define DISPLAY_ROTATION 0
#endif

// Minimum interval between e-ink refreshes.
#ifndef EINK_MIN_PRESENT_INTERVAL_MS
#define EINK_MIN_PRESENT_INTERVAL_MS 1000
#endif

// ============================================================================
// Backlight Configuration
// ============================================================================
// Enable backlight control (typically via PWM).
#ifndef HAS_BACKLIGHT
#define HAS_BACKLIGHT false
#endif

// LEDC channel used for backlight PWM.
#ifndef TFT_BACKLIGHT_PWM_CHANNEL
#define TFT_BACKLIGHT_PWM_CHANNEL 0  // LEDC channel for PWM control
#endif

// ESP_Panel (QSPI) display driver: prefer internal RAM for the byte-swap buffer.
// Default: true. Some panel buses are more reliable with internal/DMA-capable buffers.
#ifndef ESP_PANEL_SWAPBUF_PREFER_INTERNAL
#define ESP_PANEL_SWAPBUF_PREFER_INTERNAL true
#endif

// ============================================================================
// Diagnostics / Telemetry
// ============================================================================
// Low-memory tripwire: when the internal heap minimum free (bytes) drops below this
// threshold, dump per-task stack watermarks once.
// Default: disabled (0). Enable per-board if you want early warning logs.
#ifndef MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES
#define MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES 0
#endif

// How often to check tripwires from the main loop.
#ifndef MEMORY_TRIPWIRE_CHECK_INTERVAL_MS
#define MEMORY_TRIPWIRE_CHECK_INTERVAL_MS 5000
#endif

// ============================================================================
// Web Portal
// ============================================================================
// Max JSON body size accepted by /api/config.
#ifndef WEB_PORTAL_CONFIG_MAX_JSON_BYTES
#define WEB_PORTAL_CONFIG_MAX_JSON_BYTES 4096
#endif

// Timeout for an incomplete /api/config upload (ms) before freeing the buffer.
#ifndef WEB_PORTAL_CONFIG_BODY_TIMEOUT_MS
#define WEB_PORTAL_CONFIG_BODY_TIMEOUT_MS 5000
#endif

#endif // BOARD_CONFIG_H

