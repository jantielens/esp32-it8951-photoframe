# Drivers (Display)

This project targets a single e‑ink display backend. The display driver exposes a small, LVGL-free HAL.

## Display driver

- **IT8951 display driver**: `it8951_display_driver.*`
  - Presents packed G4 buffers to the panel
  - Supports full-screen and region updates

The selected display driver is compiled via:
- `src/app/display_drivers.cpp`

## Touch drivers

Touch drivers have been removed for this project.
