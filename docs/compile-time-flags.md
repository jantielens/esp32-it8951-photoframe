# Compile-Time Flags Report

This document is a template. Sections marked with `COMPILE_FLAG_REPORT` markers are auto-updated by `tools/compile_flags_report.py`.

## How to update

- Update this doc:
  - `python3 tools/compile_flags_report.py md --out docs/compile-time-flags.md`
- Print active flags during a build (example):
  - `python3 tools/compile_flags_report.py build --board cyd-v2`

## Build system notes

- Some Arduino libraries are compiled as separate translation units and may not see macros that are only defined in `src/boards/<board>/board_overrides.h`.
- The build script propagates a small allowlist of board override defines into global compiler flags so libraries are compiled with the same values.
  - Currently allowlisted:
    - `CONFIG_ASYNC_TCP_STACK_SIZE`
    - TFT_eSPI essentials for clean/CI builds (pins + SPI frequencies + controller/bus flags)
  - For TFT_eSPI specifically, `build.sh` also supports a per-board `src/boards/<board>/User_Setup.h` which is force-included for that board (so the build does not depend on a locally modified Arduino library install).

## Flags (generated)

<!-- BEGIN COMPILE_FLAG_REPORT:FLAGS -->
Total flags: 40

### Features (HAS_*)

- **HAS_BACKLIGHT** default: `false` — Enable backlight control (typically via PWM).
- **HAS_BUILTIN_LED** default: `false` — Enable built-in status LED support.
- **HAS_DISPLAY** default: `false` — Enable display support.
- **HAS_MQTT** default: `true` — Enable MQTT and Home Assistant integration.

### Selectors (*_DRIVER)

- **DISPLAY_DRIVER** default: `DISPLAY_DRIVER_IT8951` (values: DISPLAY_DRIVER_IT8951) — Select the display HAL backend.

### Hardware (Geometry)

- **DISPLAY_HEIGHT** default: `(no default)` — Display framebuffer height in pixels.
- **DISPLAY_ROTATION** default: `0` — Display rotation (0=portrait, 2=180°).
- **DISPLAY_WIDTH** default: `(no default)` — Display framebuffer width in pixels.

### Hardware (Pins)

- **BUTTON_PIN** default: `(no default)` — Button (wakeup + long press)
- **DISPLAY_POWER_EN_PIN** default: `(no default)` — Uses an RTC-capable GPIO so we can hold it LOW in deep sleep.
- **IT8951_BUSY_PIN** default: `(no default)` — IT8951 busy pin.
- **IT8951_CS_PIN** default: `(no default)` — IT8951 chip select (CS).
- **IT8951_DC_PIN** default: `(no default)` — IT8951 data/command pin (not present on this breakout).
- **IT8951_MISO_PIN** default: `(no default)` — IT8951 SPI MISO pin.
- **IT8951_MOSI_PIN** default: `(no default)` — IT8951 SPI MOSI pin.
- **IT8951_RST_PIN** default: `(no default)` — IT8951 reset pin.
- **IT8951_SCK_PIN** default: `(no default)` — IT8951 SPI SCK pin.
- **LED_PIN** default: `2` — GPIO for the built-in LED (only used when HAS_BUILTIN_LED is true).
- **SD_CS_PIN** default: `(no default)` — SD card chip select (CS).
- **SD_MISO_PIN** default: `(no default)` — SD card MISO pin.
- **SD_MOSI_PIN** default: `(no default)` — SD card MOSI pin.
- **SD_POWER_PIN** default: `(no default)` — SD card power enable pin (HIGH = on).
- **SD_SCK_PIN** default: `(no default)` — SD card clock (SCK) pin.
- **WAKE_BUTTON2_PIN** default: `(no default)` — RTC-capable on ESP32-S2 (0-21).

### Limits & Tuning

- **EINK_MIN_PRESENT_INTERVAL_MS** default: `1000` — Minimum interval between e-ink refreshes.
- **HEALTH_HISTORY_PERIOD_MS** default: `5000` — Sampling cadence for the device-side history (ms). Default aligns with UI poll.
- **MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES** default: `0` — Default: disabled (0). Enable per-board if you want early warning logs.
- **WEB_PORTAL_CONFIG_BODY_TIMEOUT_MS** default: `5000` — Timeout for an incomplete /api/config upload (ms) before freeing the buffer.
- **WEB_PORTAL_CONFIG_MAX_JSON_BYTES** default: `4096` — Max JSON body size accepted by /api/config.
- **WIFI_MAX_ATTEMPTS** default: `3` — Maximum WiFi connection attempts at boot before falling back.

### Other

- **ESP_PANEL_SWAPBUF_PREFER_INTERNAL** default: `true` — Default: true. Some panel buses are more reliable with internal/DMA-capable buffers.
- **HEALTH_HISTORY_ENABLED** default: `1` — Default: enabled.
- **HEALTH_HISTORY_SAMPLES** default: `((HEALTH_HISTORY_SECONDS * 1000) / HEALTH_HISTORY_PERIOD_MS)` — Derived number of samples.
- **HEALTH_HISTORY_SECONDS** default: `300` — How much client-side history (sparklines) to keep.
- **HEALTH_POLL_INTERVAL_MS** default: `5000` — How often the web UI polls /api/health.
- **IT8951_VCOM** default: `(no default)` — IT8951 VCOM setting from the panel spec (e.g. -1.53V => 1530).
- **LED_ACTIVE_HIGH** default: `true` — LED polarity: true if HIGH turns the LED on.
- **MEMORY_TRIPWIRE_CHECK_INTERVAL_MS** default: `5000` — How often to check tripwires from the main loop.
- **PROJECT_DISPLAY_NAME** default: `"ESP32 Device"` — Human-friendly project name used in the web UI and device name (can be set by build system).
- **TFT_BACKLIGHT_PWM_CHANNEL** default: `0` — LEDC channel used for backlight PWM.
<!-- END COMPILE_FLAG_REPORT:FLAGS -->

## Board Matrix: Features (generated)

Legend: ✅ = enabled/true, blank = disabled/false, ? = unknown/undefined

<!-- BEGIN COMPILE_FLAG_REPORT:MATRIX_FEATURES -->
| board-name | HAS_BACKLIGHT | HAS_BUILTIN_LED | HAS_DISPLAY | HAS_MQTT |
| --- | --- | --- | --- | --- |
| esp32s2-photoframe-it8951 |  |  | ✅ | ✅ |
<!-- END COMPILE_FLAG_REPORT:MATRIX_FEATURES -->

## Board Matrix: Selectors (generated)

<!-- BEGIN COMPILE_FLAG_REPORT:MATRIX_SELECTORS -->
| board-name | DISPLAY_DRIVER |
| --- | --- |
| esp32s2-photoframe-it8951 | DISPLAY_DRIVER_IT8951 |
<!-- END COMPILE_FLAG_REPORT:MATRIX_SELECTORS -->

## Usage Map (preprocessor only, generated)

<!-- BEGIN COMPILE_FLAG_REPORT:USAGE -->
- **HAS_BACKLIGHT**
  - src/app/board_config.h
  - src/app/web_portal_config.cpp
  - src/app/web_portal_display.cpp
  - src/app/web_portal_display.h
  - src/app/web_portal_routes.cpp
- **HAS_BUILTIN_LED**
  - src/app/board_config.h
- **HAS_DISPLAY**
  - src/app/app.ino
  - src/app/board_config.h
  - src/app/device_telemetry.cpp
  - src/app/display_drivers.cpp
  - src/app/display_manager.cpp
  - src/app/ha_discovery.cpp
  - src/app/it8951_renderer.cpp
  - src/app/portal_controller.cpp
  - src/app/web_portal_config.cpp
  - src/app/web_portal_device_api.cpp
  - src/app/web_portal_display.cpp
  - src/app/web_portal_display.h
  - src/app/web_portal_routes.cpp
- **HAS_MQTT**
  - src/app/app.ino
  - src/app/board_config.h
  - src/app/config_manager.cpp
  - src/app/device_telemetry.cpp
  - src/app/ha_discovery.cpp
  - src/app/ha_discovery.h
  - src/app/mqtt_manager.cpp
  - src/app/mqtt_manager.h
- **DISPLAY_DRIVER**
  - src/app/board_config.h
  - src/app/display_drivers.cpp
  - src/app/display_manager.cpp
- **DISPLAY_HEIGHT**
  - src/app/board_config.h
- **DISPLAY_ROTATION**
  - src/app/board_config.h
- **DISPLAY_WIDTH**
  - src/app/board_config.h
- **EINK_MIN_PRESENT_INTERVAL_MS**
  - src/app/board_config.h
- **ESP_PANEL_SWAPBUF_PREFER_INTERNAL**
  - src/app/board_config.h
- **HEALTH_HISTORY_ENABLED**
  - src/app/app.ino
  - src/app/board_config.h
  - src/app/health_history.cpp
  - src/app/web_portal_device_api.cpp
  - src/app/web_portal_routes.cpp
- **HEALTH_HISTORY_PERIOD_MS**
  - src/app/board_config.h
- **HEALTH_HISTORY_SAMPLES**
  - src/app/board_config.h
- **HEALTH_HISTORY_SECONDS**
  - src/app/board_config.h
- **HEALTH_POLL_INTERVAL_MS**
  - src/app/board_config.h
- **IT8951_MISO_PIN**
  - src/app/it8951_renderer.cpp
- **IT8951_MOSI_PIN**
  - src/app/it8951_renderer.cpp
- **IT8951_SCK_PIN**
  - src/app/it8951_renderer.cpp
- **IT8951_VCOM**
  - src/app/it8951_renderer.cpp
- **LED_ACTIVE_HIGH**
  - src/app/board_config.h
- **LED_PIN**
  - src/app/board_config.h
- **MEMORY_TRIPWIRE_CHECK_INTERVAL_MS**
  - src/app/board_config.h
- **MEMORY_TRIPWIRE_INTERNAL_MIN_BYTES**
  - src/app/board_config.h
  - src/app/device_telemetry.cpp
- **PROJECT_DISPLAY_NAME**
  - src/app/board_config.h
- **TFT_BACKLIGHT_PWM_CHANNEL**
  - src/app/board_config.h
- **WAKE_BUTTON2_PIN**
  - src/app/app.ino
- **WEB_PORTAL_CONFIG_BODY_TIMEOUT_MS**
  - src/app/board_config.h
- **WEB_PORTAL_CONFIG_MAX_JSON_BYTES**
  - src/app/board_config.h
- **WIFI_MAX_ATTEMPTS**
  - src/app/board_config.h
<!-- END COMPILE_FLAG_REPORT:USAGE -->
