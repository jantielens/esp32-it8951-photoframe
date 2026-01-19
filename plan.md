# ESP32 Photoframe

## Hardware Overview
- Lilygo ESP32-S2 v1.1
- Waveshare 1872×1404 7.8inch E-Ink display with IT8951 controller
- Adafruit MiniBoost 5V @ 1A (TPS61023) planned for Phase 2 to wake the ESP32 periodically

## Sample code
A very early POC was made, which can be found in /sample.

## Non-functional requirements
- Low power consumption (use deep sleep between updates), the device will be battery powered

## Functional requirements

### Phase 1:
Device boots, selects a random image from the SD card, displays it on the display and goes to deep sleep. (replicates the /sample functionality)

Implementation plan:

1) Confirm constraints + target behavior
- Image format: BMP only (must-have). JPEG can be added later (nice-to-have).
- Use the already-working wiring + SPI bus + SD folder behavior from `/sample`.
- Power: after a successful display refresh, go to deep sleep for 30 seconds (hardcoded) and repeat.

2) Create a dedicated board target for the photoframe
- Add `src/boards/esp32s2-photoframe-it8951/board_overrides.h` with the IT8951 + SD pin mapping used by the sample.
- Keep all hardware pins and VCOM in one place (board override), not scattered through code.
- Remove all other boards and board overrides under `src/boards/` to keep the project single-target:
	- Target hardware is only: Lilygo ESP32-S2 v1.1 + Waveshare 1872×1404 7.8" E-Ink (IT8951)
	- After cleanup, `src/boards/` should contain only `esp32s2-photoframe-it8951/`.

3) Keep Phase-1 firmware minimal (no LVGL, no portal, no MQTT)
- Implement a simple, synchronous boot flow in `app.ino` that does only: SD + BMP + e-ink + sleep.
- Avoid adding new compile-time flags for Phase-1; keep the logic direct and minimal.
- This does NOT rule out reintroducing LVGL + AP portal + MQTT in Phase-2.

4) Implement an SD card photo picker module
- Scan `/` (or a dedicated folder like `/photos`) for files ending in `.bmp` (case-insensitive).
- Choose uniformly at random.
- Return a stable path string (avoid returning pointers to transient buffers).
- Define failure behavior:
	- If no BMPs found: print error to Serial and sleep for 30 seconds.
	- If SD init fails: print error to Serial and sleep for 30 seconds.

5) Implement an IT8951 e-ink renderer module
- Use the same library stack as the working sample (`GxEPD_HD` + `GxIT8951`).
- Port only the required BMP draw path from the sample:
	- Render full-screen BMP at (0,0)
	- Refresh the display
	- Hibernate the display
- Keep the renderer API small, e.g. `bool renderBmpFromSd(const char* path)`.

6) Wire the Phase-1 flow in `src/app/app.ino`
- Boot → init Serial/logging → enable SD power (if needed) → init SD → seed RNG → pick BMP → render → deep sleep 30 seconds.
- Keep this flow synchronous and simple.

7) Build + run checklist
- `./setup.sh` (once)
- `./build.sh esp32s2-photoframe-it8951`
- `./upload.sh esp32s2-photoframe-it8951`
- Verify on serial:
	- SD mounts
	- A BMP filename is selected
	- Display updates
	- Enters deep sleep for 30 seconds

8) Optional (post-Phase-1): JPEG support
- Add JPEG only if needed (bigger scope: decoding, memory constraints, dithering/gray mapping).

Cleanup strategy (Phase-1 vs Phase-2)

- Keep (Phase-2 relevant): build scripts, board override system, `config.sh`, library management, release tooling, docs structure.
- Keep but do not use in Phase-1 (do not delete yet): web portal, MQTT, LVGL/touch subsystems.
- Remove now (project is single-target): all non-target board override folders under `src/boards/`.
- OK to remove now if truly unused for Phase-2: extra LCD driver examples and demo screens that are unrelated to e-ink/IT8951.
- Rule of thumb: if a file only exists to support “web portal + LCD + touch UI”, keep it for Phase-2; if it’s a one-off demo or unused board that won’t be used by this photoframe, remove it.

### Phase 2:
Use this template in this repo to have a more production ready project (to be completed and refined)
- Use the Adafruit MiniBoost 5V @ 1A (TPS61023) to wake the ESP32 periodically (e.g. every hour)
- Reintroduce AP mode + config portal + MQTT integration as needed
- Optional LVGL UI for splash/progress/status screens
- Web portal to manage images on the SD card (upload, delete, list)
- Messages on the screen (when booting, connecting to WiFi, errors ...)
- Instead of loading images from SD card, download them from a web server (with caching on SD card, optionally), also configurable from the web portal