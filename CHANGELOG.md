# Changelog

## 2.0.0 - unreleased

### Added
- `lib/leaf_can`: hardware-independent EV-CAN library (frame type, IDs, CRC-8, signal codecs, inverter
  upgrade). It is packaged for Arduino, PlatformIO, ESP-IDF and CMake.
- `lib/bridge_settings`: settings model, strict JSON parser and NVS key map shared by both firmwares.
- `firmware-idf/`: ESP-IDF 6.1 firmware with the on-chip TWAI node driver, its own MCP2515 driver, a single
  bridge task, gzip-embedded web UI, `/api/status`, password-protected OTA with automatic rollback, and a
  task watchdog.
- `/api/status` JSON endpoint in both firmwares.
- Tests: unit tests, a differential test against the v1.3.0 handler, and a QEMU end-to-end test of the
  ESP-IDF firmware. Also a CI workflow and `tools/ci_local.sh`.
- PlatformIO project for the Arduino firmware, with pinned library versions.

### Fixed
- Scaled torque requests are saturated instead of wrapping into the sign bit, where full throttle
  could become regen.
- The AC-charging guard works again (`charging_state` is read from 0x1F2).
- Choosing *e-NV200* no longer stops inverter → VCM forwarding.
- Settings changes and OTA updates are refused unless the car is in P.
- Settings parser buffer overflow on long request bodies.
- OTA is password protected.
- The VCM-bus receive path no longer runs in an ISR that races the main loop on the TX buffers.
- ESP32 rev3 bit rate is handled by the ESP-IDF TWAI driver, so the `1000E3` workaround is no longer needed.
- 0x5CD is no longer sent to the VCM bus. Translation and emulation are direction-aware.
- *Inverter: Disabled* leaves torque frames untouched.
- Regen scaling is exact (v1.3.0 was off by up to 0.25 Nm).

### Changed
- The Arduino sketch is now `firmware/firmware.ino`, so the Arduino IDE accepts the folder name.
- The web UI moved to `web/` and posts `application/json`; the v1.x form encoding is still accepted.
  Settings that are stored but not active are labelled on the page.
- Wi-Fi, OTA and pin configuration moved to `firmware/config.h` and ESP-IDF menuconfig.
- Arduino libraries updated to maintained versions (ESP32Async AsyncTCP / ESPAsyncWebServer, ElegantOTA 3).

### Removed
- Vendored library ZIPs, the duplicate schematic, the unused config portal, the empty e-NV200 files,
  the fully commented-out inverter file, the 0x1DB "eco screen" debug code, the custom OTA page and its scripts.

## 1.3.0
Last Arduino-only version by Adam Saiyad and Julius Calzada (last dated change 2023-09-15).
