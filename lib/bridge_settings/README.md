# bridge_settings

Settings model of the Esp32LeafInverterBridge web UI, shared by the Arduino and ESP-IDF firmwares.

- One table describes every setting: its web-form name, its **NVS key** (unchanged since v1.x, namespace
  `ESP32`) and its options, in radio-button order. The stored value is the option index, as in v1.x.
- `bridge_settings_parse_json()` parses the body posted by `web/index.html`. It is strict and bounded:
  malformed JSON, unknown option values and over-long tokens are rejected with a message, and the current
  settings are left untouched. Missing fields keep their value and unknown keys are ignored.
- `bridge_settings_radio_id()` returns the id the page's WebSocket handler expects (`Inverter2`, …).
- `bridge_settings_to_inverter_config()` maps the settings onto a `leaf_can` configuration.

Depends on [`leaf_can`](../leaf_can). Tests: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.
