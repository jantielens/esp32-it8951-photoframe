# Phase 2 Plan

## Project description & scope
- Battery-powered ESP32 photo frame with Waveshare 7.8" E-Ink (IT8951)
- Phase 2 focuses on SD-card image management (server download comes later)
- Initial onboarding via AP portal to set up WiFi and device settings
- Normal usage is mostly deep sleep for maximum battery life
- Wake behavior:
  - Single button click: wake, show next image, return to deep sleep
  - Button hold: enter config mode (STA portal for managing images/settings)
- Image selection is configurable: random or sequential (filename-sorted)
- LVGL-based UI is out of scope for phase 2 (future phase)

## In scope / out of scope (proposal)
**In scope**
- SD card image management for G4 assets stored in SD root
- Button-driven wake behavior (short press = next image, long press = config mode)
- Configurable image selection mode (random or sequential)
- Configurable sleep timeout (default 60s)
- Portal image management (list, upload, delete, display now)

**Out of scope**
- Server download / cloud sync
- LVGL-based UI
- Album/folder organization or scheduling
- New power hardware features beyond button + timer wake

## Modes
- AP mode: first-boot or WiFi-failure fallback; captive portal for setup
- Config mode: STA mode (entered via long-press) with full portal for image management & settings
- Normal mode: default; wake on button, display image, return to deep sleep (configurable timer + button wakeup source)

## Hardware assumptions
- Button pin: `#define BUTTON_PIN 21`
- Wake sources: button + timer
- Long press duration: 1500 ms

## Power / sleep behavior
- Sleep timeout is configurable (default 60 seconds)
- Wake sources: button and timer
- WiFi retry policy handled by existing template code

## SD card requirements
- Images live under `/perm/` and `/temp/`
- Image format: preprocessed G4 files (current pipeline)
- Max filename length: 127 characters (plus leading `/`)
- Allowed filenames: standard FAT 8.3 and long filenames; reject names that exceed limit

## Image selection
- Modes:
  - Random
  - Sequential (sorted by filename)
- Sequential mode uses a deterministic, sorted list of filenames (KISS ordering)

## Portal requirements (phase 2)
- List images on SD (G4 only)
- Upload image (G4)
- Delete image
- Display image immediately (without boot cycle)
  - Image stays on screen until next boot cycle (next/random image shown then)
  - Sequential index is advanced to the displayed image

## Config fields & defaults (proposal)
- `sleep_timeout_seconds` (uint16_t): default 60
- `image_selection_mode` (enum/string): `random` (default) or `sequential`
- `last_image_index` (uint32_t): default 0 (sequential mode)
  - Stored in deep-sleep retained memory (avoid NVS if possible)
  - If retention is lost, start from the beginning
- `long_press_ms` (uint16_t): default 1500 (keep configurable for tuning)

## State machine (phase 2)
```text
BOOT
  ├─ if WiFi config missing OR WiFi connect fails → AP_MODE (captive portal)
  ├─ if long-press detected → CONFIG_MODE (STA portal)
  └─ else → NORMAL_MODE

AP_MODE
  └─ user saves config → reboot → BOOT

CONFIG_MODE
  ├─ portal actions (list/upload/delete/display)
  └─ user exits (timeout or reboot) → BOOT

NORMAL_MODE
  ├─ select image (random/sequential)
  ├─ render image to IT8951
  └─ deep sleep (timer + button wake)
```

## Acceptance criteria (proposal)
- Short press shows a new image and returns to deep sleep
- Long press enters config mode and exposes the full portal
- If WiFi config is missing or WiFi fails to connect, AP mode starts
- Image selection mode persists across reboots
- Sequential mode walks sorted filenames and wraps to start
- Portal can list, upload, delete, and display G4 images immediately
- “Display now” keeps the image until next boot cycle
- Device returns to deep sleep after configurable timeout

## Manual test ideas (high-level)
- Button short press shows next image and sleeps
- Button long press enters config mode
- WiFi failure forces AP mode
- Upload, list, delete, and “display now” from portal
- Sequential vs random selection behavior across reboots

## Additional constraints
- Upload constraints: max 2 MB, reject non-`.g4` files
- Random mode is purely random (no persistence)
- Long-press detection only during boot
- Config mode exit is via portal-triggered reboot
- SD errors: log to serial and return to deep sleep
- If no G4 images exist: log to serial and return to deep sleep
- WiFi failure threshold uses existing template behavior
- Upload name conflicts: overwrite existing file
- Button debounce: not required (button used only for wake source + long-press check)

## Implementation plan:
### [TASK001] Config + retained state wiring

Description:
Add phase-2 config fields and RTC retained state for sequential index (no NVS). Define defaults and retention fallback behavior.

Scope:
Config manager fields, defaults, and RTC retained storage.

Files/Modules:
`src/app/config_manager.*`, `src/app/app.ino` (or new small module for RTC state).

Dependencies:
None.

Acceptance criteria:
- Fields exist and defaults match the plan.
- Retained index survives deep sleep; resets to 0 when retention is lost.

Manual tests:
- Reboot vs deep sleep retains index as expected.

Implementation notes:
Implemented: added phase-2 config fields in config_manager and RTC retained image index helper in rtc_state.

### [TASK002] Button wake + long-press handling in boot

Description:
Configure wake sources and detect long-press during boot only.

Scope:
GPIO wake config, long-press check (1500 ms), branch to config vs normal mode.

Files/Modules:
`src/app/app.ino`.

Dependencies:
[TASK001].

Acceptance criteria:
- Short press enters normal flow.
- Long press enters config mode.

Manual tests:
- Observe logs for short vs long press path selection.

Implementation notes:
Implemented: early boot long-press detection, button wake setup, and config mode portal flow with WiFi/AP fallback; BUTTON_PIN set in board overrides.

### [TASK003] SD image indexer + selection logic

Description:
Implement SD root scan for `.g4`, KISS filename sorting, random/sequence selection, and sequential wrap.

Scope:
List images, select image by mode, handle empty SD case.

Files/Modules:
`src/app/sd_photo_picker.*` (extend or add new helper).

Dependencies:
[TASK001].

Acceptance criteria:
- Random mode selects any file.
- Sequential mode walks sorted list and wraps.
- Empty SD logs and sleeps.

Manual tests:
- Verify order and wrap with a known filename set.

Implementation notes:
Implemented: G4 selection in sd_photo_picker with KISS sorting, random/sequential modes, and RTC retained index wiring in app.ino.

### [TASK004] Normal-mode render + sleep flow

Description:
Integrate selection with IT8951 render path and deep sleep timeout (default 60s).

Scope:
Call render, log errors, enter deep sleep with timer + button wake.

Files/Modules:
`src/app/app.ino`, `src/app/it8951_renderer.*`.

Dependencies:
[TASK002], [TASK003].

Acceptance criteria:
- Selected image renders.
- Device sleeps after timeout.

Manual tests:
- Observe render + sleep cycle.

Implementation notes:
Implemented: configurable sleep timeout applied to deep sleep flow in app.ino.

### [TASK005] Portal API: list/upload/delete/display-now

Description:
Add REST endpoints for SD image management, enforce `.g4` and 2 MB limit, overwrite on conflict.

Scope:
API handlers, SD file I/O, immediate display without boot cycle, sequential index update.

Files/Modules:
`src/app/web_portal*.cpp/h`, new or existing image management module.

Dependencies:
[TASK001], [TASK003], [TASK004].

Acceptance criteria:
- List shows SD `.g4` files.
- Upload rejects non-`.g4` and >2 MB.
- Upload overwrites on conflict.
- Delete removes file.
- Display-now shows image and advances sequential index.

Manual tests:
- Exercise list/upload/delete/display-now via portal.

Implementation notes:
Implemented: SD image API (list/upload/delete/display) with 2MB limit, overwrite behavior, and sequential index update on display-now.

### [TASK006] Portal UI wiring (if needed)

Description:
Expose SD management in the portal UI (list/upload/delete/display-now).

Scope:
HTML/JS changes only; reuse existing portal infrastructure.

Files/Modules:
`src/app/web/*.html`, `src/app/web/portal.js`.

Dependencies:
[TASK005].

Acceptance criteria:
- UI can list, upload, delete, and display images.

Manual tests:
- Verify operations from the UI.

Implementation notes:
Implemented: SD image management UI and client-side actions on the portal home page, including full-page overlay during upload/display.

### [TASK007] Error handling + logging pass

Description:
Ensure SD missing/corrupt, render failures, and API errors log to serial and sleep/return safely.

Scope:
Consistent logging and safe fallbacks.

Files/Modules:
`src/app/app.ino`, SD helpers, portal handlers.

Dependencies:
[TASK003], [TASK004], [TASK005].

Acceptance criteria:
- Error cases log and sleep/return without crashes.

Manual tests:
- Simulate missing SD and corrupt file.

Implementation notes:
Implemented: added SD selection and SD image API error logging for missing SD, invalid inputs, and render/upload failures.

### [TASK008] Sanity pass + manual verification

Description:
Manual checks across modes and core flows based on the plan’s test ideas.

Scope:
Only manual verification; no new automated tests.

Files/Modules:
N/A.

Dependencies:
All prior tasks.

Acceptance criteria:
- Manual tests pass per plan.

Manual tests:
- Use the “Manual test ideas” section above.

Implementation notes:
Not implemented yet.


