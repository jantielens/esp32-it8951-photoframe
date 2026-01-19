# ESP32 iT8951 Photoframe

Project-specific firmware for a single target: Lilygo ESP32-S2 v1.1 + Waveshare 7.8" E-Ink (IT8951, 1872x1404).

## v1.0.0 MVE (Phase 1)
- Boot → pick random BMP from SD → render to e-ink → deep sleep (30s).
- Minimal firmware surface: no WiFi/AP portal/MQTT/LVGL.
- SD + display wiring and SPI settings follow /sample.

## Hardware
- Lilygo ESP32-S2 v1.1
- Waveshare 7.8" E-Ink IT8951 (1872×1404)

## Build
```bash
./setup.sh
./build.sh esp32s2-photoframe-it8951
```

## Upload
```bash
./upload.sh esp32s2-photoframe-it8951
```

## Monitor
```bash
./monitor.sh
```

## SD Card Expectations
- BMP files in the root of the SD card (case-insensitive .bmp).
- One random BMP is selected per boot.

## Notes
- The IT8951 driver uses GxEPD2. Grayscale rendering is 16 levels.
- Future phases can reintroduce AP mode, portal, MQTT, and LVGL.
