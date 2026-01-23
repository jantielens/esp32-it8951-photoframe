#ifndef BOARD_OVERRIDES_FEATHERS3D_IT8951_H
#define BOARD_OVERRIDES_FEATHERS3D_IT8951_H

// ============================================================================
// Board Overrides: feathers3d
// Target: Unexpected Maker FeatherS3[D] (esp32:esp32:um_feathers3)
// Notes:
// - Pin choices follow the FeatherS3[D] pinout (esp32s3.com/feathers3d.html).
// - This file encodes the assumed wiring between the FeatherS3[D], SD, and the
//   IT8951 e-ink HAT. Adjust the CS/BUSY/etc pins to match your wiring.
// ============================================================================

// ---------------------------------------------------------------------------
// Built-in LED (FeatherS3[D] blue LED)
// ---------------------------------------------------------------------------
#define HAS_BUILTIN_LED true
#define LED_PIN 13
#define LED_ACTIVE_HIGH true

// ---------------------------------------------------------------------------
// Boot behavior
// ---------------------------------------------------------------------------
// USB CDC boards re-enumerate on deep sleep, which can look like reboot loops.
// Keep the device running by default until configured.
#define DEFAULT_ALWAYS_ON true

// ---------------------------------------------------------------------------
// SD Card (separate SPI bus)
// ---------------------------------------------------------------------------
// IMPORTANT: Do NOT share SCK/MOSI/MISO with the IT8951 on FeatherS3[D].
// In practice, the IT8951 board can load/distort SCK enough to make SD init
// hang or fail. Use a separate pin trio (S2-style).
//
// Default pin trio (adjust to match your wiring/pinout availability):
//   MOSI = IO11 (D13)
//   SCK  = IO12 (A3)
//   MISO = IO7  (D11)
#define SD_MOSI_PIN 11
#define SD_SCK_PIN 12
#define SD_MISO_PIN 7

// Chip select for your SD socket/breakout.
// Chosen default: IO33 (D5) — adjust if you wired differently.
#define SD_CS_PIN 33

// Optional SD power enable (HIGH = on). If not wired, leave disabled.
#define SD_POWER_PIN -1

// SD cards + breakouts vary widely; start conservative for reliability.
#define SD_SPI_FREQUENCY_HZ 20000000

// SD uses its own SPI peripheral/pins (not the global SPI used by IT8951).
#define SD_USE_ARDUINO_SPI false

// ---------------------------------------------------------------------------
// IT8951 E-Ink
// ---------------------------------------------------------------------------
// SPI pins (match the FeatherS3[D] SPI pin trio above).
#define IT8951_SCK_PIN 36
#define IT8951_MOSI_PIN 35
#define IT8951_MISO_PIN 37

// Chip select (CS) for the IT8951 HAT.
// Chosen default: IO10 (D12) — adjust if you wired differently.
#define IT8951_CS_PIN 10

// IT8951 data/command pin (not present on this breakout).
#define IT8951_DC_PIN -1

// Reset + busy pins.
// Chosen defaults: RST=IO38 (D6), BUSY=IO5 (A5) — adjust if you wired differently.
#define IT8951_RST_PIN 38
#define IT8951_BUSY_PIN 5

// IT8951 VCOM setting from the panel spec (e.g. -1.53V => 1530).
#define IT8951_VCOM 1530

// Optional: display rail enable (HIGH = on). If you control the 5V boost EN,
// set this to your GPIO; otherwise disable.
#define DISPLAY_POWER_EN_PIN 17

// ---------------------------------------------------------------------------
// Display (IT8951 7.8")
// ---------------------------------------------------------------------------
#define DISPLAY_DRIVER DISPLAY_DRIVER_IT8951
#define DISPLAY_WIDTH 1872
#define DISPLAY_HEIGHT 1404
#define DISPLAY_ROTATION 2

// ---------------------------------------------------------------------------
// Button (wakeup + long press)
// ---------------------------------------------------------------------------
// Use a free GPIO that is NOT a strapping pin.
// Chosen default: IO14 (A2).
#define BUTTON_PIN 14

#endif // BOARD_OVERRIDES_FEATHERS3D_IT8951_H
