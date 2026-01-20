# Drivers (Display + Touch)

This project targets a single e‑ink display backend. The display driver is implemented as an LVGL framebuffer adapter and lives alongside optional touch drivers.

## Display driver

- **IT8951 LVGL driver**: `it8951_lvgl_driver.*`
  - Buffered render mode (RGB565 → G4 conversion)
  - `present()` pushes the G4 framebuffer to the panel

The selected display driver is compiled via:
- `src/app/display_drivers.cpp`

## Touch drivers

Touch drivers are optional and compiled via:
- `src/app/touch_drivers.cpp`
