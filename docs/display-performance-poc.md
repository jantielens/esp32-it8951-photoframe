# Display Performance PoC Log

This document tracks performance measurements for different display pipelines (drivers, render paths, and optimizations) on the ESP32 iT8951 photoframe.

## Hardware
- Board: Lilygo ESP32-S2 v1.1
- Display: Waveshare 7.8" E-Ink IT8951 (1872×1404)
- Power: USB (Phase-1)

**Logging tags**
- `SD`: SD init, scan, pick
- `EINK`: BMP decode, row writes, refresh, total render
- `PHASE1`: boot + sleep

## [00] Baseline

**Pipeline**
- SD → BMP decode → 16-gray quantization → IT8951 `writeNative()` (8bpp) → `refresh(false)`

**Baseline results (2026-01-19)**
- Boot log: 241 ms
- SD Init: 7 ms
- SD Scan: 2372 ms
- SD Pick: 2372 ms
- Display Init: 1316 ms
- Rows Write: 7555 ms
- Refresh: 628 ms
- BMP Decode (total): 12027 ms
- Render Total: 12041 ms
- BMP count: 116
- Picked path: /20140716-P7164162.bmp

## [01] Chunked writeNative (8 rows)

**Pipeline**
- SD → BMP decode → 16-gray quantization → IT8951 `writeNative()` (8bpp) → `refresh(false)`

**Results (2026-01-19)**
- Boot log: 241 ms
- SD Init: 7 ms
- SD Scan: 2413 ms
- SD Pick: 2413 ms
- Display Init: 1315 ms
- Rows Write: 6164 ms
- Refresh: 628 ms
- BMP Decode (total): 10637 ms
- Render Total: 10664 ms
- BMP count: 116
- Picked path: /L1007137.bmp

## [02] Chunked writeNative (16 rows)

**Results (2026-01-19)**
- Rows Write: 6214 ms

## [03] Chunked writeNative (4 rows)

**Results (2026-01-19)**
- Rows Write: 6506 ms

## [02b] Chunked writeNative (8 rows) + no clearScreen

**Results (2026-01-19)**
- Rows Write: 9993 ms
- Refresh: 628 ms
- BMP Decode (total): 10633 ms
- Render Total: 10648 ms
- BMP count: 116
- Picked path: /20141004-PA046272.bmp

## [02c] Chunked writeNative (8 rows) + clearScreen restored

**Results (2026-01-19)**
- Rows Write: 6164 ms
- Refresh: 628 ms
- BMP Decode (total): 10636 ms
- Render Total: 10660 ms
- BMP count: 116
- Picked path: /20250303-DSC09950.bmp

## [04] Luminance weights (77/150/29)

**Results (2026-01-19)**
- Rows Write: 6258 ms
- Refresh: 628 ms
- BMP Decode (total): 10731 ms
- Render Total: 10759 ms
- BMP count: 116
- Picked path: /L1007502.bmp

## [05] Sequential row read (no per-row seek)

**Results (2026-01-19)**
- Rows Write: 6148 ms
- Refresh: 628 ms
- BMP Decode (total): 10621 ms
- Render Total: 10643 ms
- BMP count: 116
- Picked path: /20230528-DSC05231.bmp

## [06] SPI write speed 24MHz

**Results (2026-01-19)**
- Rows Write: 6145 ms
- Refresh: 628 ms
- BMP Decode (total): 10617 ms
- Render Total: 10630 ms
- BMP count: 116
- Picked path: /20130801-DSC_3729.bmp

## [07] SPI write speed 40MHz

**Results (2026-01-19)**
- Rows Write: 6145 ms
- Refresh: 628 ms
- BMP Decode (total): 10618 ms
- Render Total: 10646 ms
- BMP count: 116
- Picked path: /L1007336.bmp

## [08] Kept baseline (8 rows + luminance weights)

**Results (2026-01-19)**
- Rows Write: 6258 ms
- Refresh: 628 ms
- BMP Decode (total): 10731 ms
- Render Total: 10759 ms
- BMP count: 116
- Picked path: /L1007502.bmp

## [09] BMP vs RAW/G4 render (on-device convert)

**Pipeline**
- SD → BMP decode → 16-gray quantization → IT8951 `writeNative()` (8bpp) → `refresh(false)`
- SD → BMP → RAW (8bpp) + G4 (4bpp packed) conversion
- SD → RAW render → `refresh(false)`
- SD → G4 render (unpack → 8bpp) → `refresh(false)`

**Results (2026-01-19)**
- Display Init: 1317 ms
- BMP Rows Write: 6235 ms
- BMP Refresh: 628 ms
- BMP Decode (total): 10707 ms
- BMP Render Total: 10734 ms
- Convert BMP → RAW+G4: 6146 ms
- RAW Rows Write: 5151 ms
- RAW Refresh: 627 ms
- RAW Render Total: 5779 ms
- G4 Rows Write: 4473 ms
- G4 Refresh: 627 ms
- G4 Render Total: 5101 ms
- Path: /L1007502.bmp (raw=/L1007502.raw, g4=/L1007502.g4)

## [10] G4-only render (preconverted on SD)

**Pipeline**
- SD → G4 read (4bpp packed) → unpack to 8bpp → IT8951 `writeNative()` → `refresh(false)`

**Results (2026-01-19)**
- Display Init: 1315 ms
- G4 Rows Write: 8306 ms
- G4 Refresh: 627 ms
- G4 Render Total: 8934 ms
- Path: /L1007502.g4 (from /L1007502.bmp)

## [11] G4-only render (packed 4bpp to IT8951)

**Pipeline**
- SD → G4 read (4bpp packed) → IT8951 4bpp load → `refresh(false)`

**Results (2026-01-19)**
- Display Init: 1315 ms
- G4 Rows Write: 1453 ms
- G4 Refresh: 633 ms
- G4 Render Total: 2086 ms
- Path: /L1007502.g4 (from /L1007502.bmp)
