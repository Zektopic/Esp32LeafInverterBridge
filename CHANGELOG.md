# Changelog

## 2.1.0 - unreleased (ESP-IDF firmware)

### Added
- New web UI, plain JavaScript with no dependencies and about 20 KB gzipped, replacing about 145 KB of
  Bootstrap/jQuery:
  - live dashboard over a WebSocket (gear, inverter mode, state of charge, charging, torque bars, both CAN buses,
    bridge latency);
  - settings with a save bar;
  - firmware update with progress and reboot tracking;
  - device page for passwords, Wi-Fi, system information, diagnostics, crash dump, restart and factory reset;
  - mobile first, light and dark theme.
- REST API: `/api/settings`, `/api/session`, `/api/login`, `/api/logout`, `/api/password`, `/api/network`,
  `/api/system`, `/api/reboot`, `/api/factory-reset`, `/api/coredump`, `/api/update`. Errors are returned as
  JSON.
- Admin password, separate from the Wi-Fi password:
  - stored as a salted PBKDF2-HMAC-SHA256 hash;
  - changes are blocked until the factory default has been replaced;
  - bearer-token sessions with an idle timeout;
  - login lockout with an increasing delay;
  - HTTP basic auth still accepted for scripts.
- Wi-Fi name and password can be changed in the UI. The access point uses WPA2/WPA3 with PMF capability.
- Captive portal: a DNS server and connectivity-check redirect, so phones open the page after joining.
- Diagnostics:
  - bus error state and TEC/REC;
  - bridge latency (avg / max / peak);
  - frame rates and queue high water;
  - task stack headroom and reset reason;
  - crash-dump download.
- Tests:
  - host tests (ASan/UBSan) for the JSON helpers and the DNS responder;
  - QEMU end-to-end test rewritten for the new API (about 100 checks);
  - Playwright browser test of the first-run flow, including a firmware update through the UI.

### Changed
- The CAN drivers are started from the bridge task, so the CAN interrupts are served by CPU 1. Wi-Fi, lwIP and
  the web server stay on CPU 0, and the CPU runs at 240 MHz.
- MCP2515 driver:
  - keeps servicing the chip while INT is low;
  - re-initialises a controller that was reset by a supply glitch;
  - keeps retrying one that is missing at boot;
  - feeds the task watchdog.
- OTA images are checked (magic, chip, project name) before anything is written to flash.
- Web assets are revalidated with ETags (`304 Not Modified`). The page is served with a strict
  Content-Security-Policy and security headers.
- Kconfig: *OTA user name / password* are replaced by *factory admin password*. *Captive portal* is new.

### Removed
- `POST /settings` and the radio-button WebSocket messages of the 2.0 page in the ESP-IDF firmware. They are
  replaced by `/api/settings` and the status feed. The Arduino firmware keeps the 2.0 page and API.

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
