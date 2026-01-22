#ifndef BOARD_OVERRIDES_PHOTOFRAME_IT8951_H
#define BOARD_OVERRIDES_PHOTOFRAME_IT8951_H

// ============================================================================
// Board Overrides: esp32s2-photoframe-it8951
// Target: Lilygo ESP32-S2 v1.1 + Waveshare 7.8" E-Ink (IT8951)
// Wiring/pins mirror the working /sample implementation.
// ============================================================================

// ---------------------------------------------------------------------------
// Phase-1 Scope (minimal firmware)
// ---------------------------------------------------------------------------
// Disable template subsystems for Phase-1 simplicity.
#define HAS_MQTT true
#define HEALTH_HISTORY_ENABLED 1

// ---------------------------------------------------------------------------
// SD Card (HSPI) - from /sample
// ---------------------------------------------------------------------------
// SD card chip select (CS).
#define SD_CS_PIN 10
// SD card MOSI pin.
#define SD_MOSI_PIN 11
// SD card clock (SCK) pin.
#define SD_SCK_PIN 12
// SD card MISO pin.
#define SD_MISO_PIN 13
// SD card power enable pin (HIGH = on).
#define SD_POWER_PIN 14

// ---------------------------------------------------------------------------
// IT8951 E-Ink (from /sample)
// ---------------------------------------------------------------------------
// Display power enable (MiniBoost EN) - HIGH = on.
// Uses an RTC-capable GPIO so we can hold it LOW in deep sleep.
#define DISPLAY_POWER_EN_PIN 16

// IT8951 chip select (CS).
#define IT8951_CS_PIN 34
// IT8951 SPI MOSI pin.
#define IT8951_MOSI_PIN 35
// IT8951 SPI MISO pin.
#define IT8951_MISO_PIN 37
// IT8951 SPI SCK pin.
#define IT8951_SCK_PIN 36
// IT8951 data/command pin (not present on this breakout).
#define IT8951_DC_PIN -1
// IT8951 reset pin.
#define IT8951_RST_PIN 38
// IT8951 busy pin.
#define IT8951_BUSY_PIN 4
// IT8951 VCOM setting from the panel spec (e.g. -1.53V => 1530).
#define IT8951_VCOM 1530

// ---------------------------------------------------------------------------
// Display (LVGL) - IT8951 panel
// ---------------------------------------------------------------------------
// Display framebuffer width in pixels.
#define DISPLAY_WIDTH 1872
// Display framebuffer height in pixels.
#define DISPLAY_HEIGHT 1404
// Display rotation (0=portrait, 2=180°).
#define DISPLAY_ROTATION 2

// ---------------------------------------------------------------------------
// Button (wakeup + long press)
// ---------------------------------------------------------------------------
#define BUTTON_PIN 21

// Optional second wakeup button (wake-only).
// Wire as normally-open to GND, same as BUTTON_PIN (active LOW).
// RTC-capable on ESP32-S2 (0-21).
#define WAKE_BUTTON2_PIN 17

#endif // BOARD_OVERRIDES_PHOTOFRAME_IT8951_H
