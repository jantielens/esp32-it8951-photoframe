# ESP32 iT8951 Photoframe

Project-specific firmware for a single target: Lilygo ESP32-S2 v1.1 + Waveshare 7.8" E-Ink (IT8951, 1872x1404).

## v1.0.0 MVE (Phase 1)
- Boot → pick random `.g4` from SD → render to e-ink → deep sleep (20s).
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
- `.g4` files in the root of the SD card.
- One random `.g4` is selected per boot.

## Image Preprocessing (JPG → G4)
Use [tools/jpg_to_g4.py](tools/jpg_to_g4.py) to convert folders of JPGs to packed 4bpp `.g4` files (1872×1404, letterboxed on white). Outputs are vertically flipped and mirrored horizontally to match panel orientation.

Examples:
```bash
python3 tools/jpg_to_g4.py /path/to/photos               # default OPT+BAYER
python3 tools/jpg_to_g4.py /path/to/photos --variant base
python3 tools/jpg_to_g4.py /path/to/photos --variant opt
python3 tools/jpg_to_g4.py /path/to/photos --variant opt-bayer
python3 tools/jpg_to_g4.py /path/to/photos --variant opt-fs
python3 tools/jpg_to_g4.py /path/to/photos --variant compare
```

## Notes
- The IT8951 driver uses GxEPD2. Grayscale rendering is 16 levels.
- G4 is a packed 4bpp grayscale format (2 pixels per byte) for fast rendering.
- A lightweight e‑ink UI renderer provides splash/progress without LVGL.
