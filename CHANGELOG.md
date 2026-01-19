# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

---

## [1.0.0] - 2026-01-19

### Added
- Minimal photoframe firmware (SD → BMP → IT8951 e-ink → deep sleep).
- Dedicated board target for esp32s2-photoframe-it8951 with sample-based pins.
- SD photo picker and IT8951 renderer modules.
- GxEPD2 dependency for IT8951 panel support.

### Changed
- Single-target focus (removed non-target board overrides).

---

## Template for Future Releases

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- New features

### Changed
- Changes to existing features

### Deprecated
- Features marked for removal

### Removed
- Removed features

### Fixed
- Bug fixes

### Security
- Security patches
```
