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
