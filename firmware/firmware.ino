//——————————————————————————————————————————————————————————————————————————————
// Esp32LeafInverterBridge - Arduino firmware
//
// Two-bus CAN bridge between the VCM side of a Nissan LEAF / e-NV200 EV-CAN and
// a 110 kW / 160 kW (EM57, 2018+) inverter. All message handling lives in the
// shared, hardware independent lib/leaf_can library; this sketch only wires it
// to the CAN controllers, the web UI and NVS.
//
// Original authors: Adam Saiyad, Julius Calzada (v1.x, 2022-2023)
// Based on the Dala inverter-upgrade firmware (github.com/dalathegreat)
// SPDX-License-Identifier: GPL-3.0-only
//
// Build: PlatformIO (see platformio.ini) or Arduino IDE / arduino-cli with
// lib/leaf_can and lib/bridge_settings installed as libraries (see README).
//——————————————————————————————————————————————————————————————————————————————
#ifndef ARDUINO_ARCH_ESP32
#error "Select an ESP32 board"
#endif

#include <Arduino.h>
#include <stdarg.h>
#include <bridge_settings.h>
#include <leaf_can.h>

#include "bridge_state.h"
#include "can_bus.h"
#include "config.h"
#include "settings_store.h"
#include "web_ui.h"

static_assert(sizeof(WIFI_AP_PASSWORD) - 1 >= 8, "WIFI_AP_PASSWORD must have at least 8 characters (WPA2)");

// Frames handled per bus per loop() pass, so one busy bus cannot starve the other.
static constexpr int kMaxFramesPerPass = 32;

// ---- state owned by loop() ---------------------------------------------------
static leaf_inverter_t g_inverter;

// ---- state shared with the web server task (guarded by g_lock) --------------
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bridge_settings_t g_settings;
static bool g_settings_pending = false;
static BridgeStatus g_status;
static volatile leaf_shifter_t g_shifter = LEAF_SHIFTER_PARK;

// printf-style debug output, compiled to nothing unless SERIAL_DEBUG is defined.
static void bridge_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bridge_log(const char *fmt, ...) {
#ifdef SERIAL_DEBUG
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
#else
  (void)fmt;
#endif
}

static void emit_frame(void *, leaf_bus_t bus, const leaf_can_frame_t *frame) {
  can_bus_send(bus, frame);
}

static void bridge_frame(leaf_bus_t from, leaf_can_frame_t &frame) {
  if (leaf_inverter_process(&g_inverter, from, &frame, emit_frame, nullptr)) {
    can_bus_send(leaf_bus_other(from), &frame);
  }
}

static void apply_pending_settings() {
  bool pending;
  bridge_settings_t s;
  portENTER_CRITICAL(&g_lock);
  pending = g_settings_pending;
  s = g_settings;
  g_settings_pending = false;
  portEXIT_CRITICAL(&g_lock);
  if (pending) {
    leaf_inverter_config_t cfg;
    bridge_settings_to_inverter_config(&s, &cfg);
    leaf_inverter_set_config(&g_inverter, &cfg);
    bridge_log("[bridge] settings applied (inverter option %u)\n", s.value[BRIDGE_SETTING_INVERTER]);
  }
}

static void publish_status() {
  BridgeStatus st;
  st.uptime_s = millis() / 1000UL;
  st.shifter = g_inverter.shifter;
  st.torque_request_vcm = g_inverter.vcm_torque_request;
  st.torque_request_out = g_inverter.torque_request_out;
  st.torque_response = g_inverter.torque_response_in;
  st.charge_status = g_inverter.charge_status;
  st.ac_charging = leaf_inverter_ac_charging(&g_inverter);
  st.soc_valid = g_inverter.soc_valid;
  st.soc_permille = g_inverter.soc_permille;
  st.inverter = g_inverter.stats;
  st.can = can_bus_stats();
  portENTER_CRITICAL(&g_lock);
  g_status = st;
  portEXIT_CRITICAL(&g_lock);
  g_shifter = st.shifter;
}

// ---- bridge_state.h ------------------------------------------------------------
BridgeStatus bridge_status() {
  portENTER_CRITICAL(&g_lock);
  BridgeStatus st = g_status;
  portEXIT_CRITICAL(&g_lock);
  return st;
}

bridge_settings_t bridge_settings() {
  portENTER_CRITICAL(&g_lock);
  bridge_settings_t s = g_settings;
  portEXIT_CRITICAL(&g_lock);
  return s;
}

bool bridge_is_parked() {
  return g_shifter == LEAF_SHIFTER_PARK;
}

SettingsResult bridge_request_settings(const bridge_settings_t &settings) {
  const bridge_settings_t current = bridge_settings();
  if (bridge_settings_equal(&settings, &current)) {
    return SettingsResult::Unchanged;
  }
  // Writing flash stalls both CPU cores for milliseconds and the new torque map
  // would take effect immediately: only allow changes while parked.
  if (!bridge_is_parked()) {
    return SettingsResult::NotParked;
  }
  if (!settings_save(&settings)) {
    return SettingsResult::StorageError;
  }
  portENTER_CRITICAL(&g_lock);
  g_settings = settings;
  g_settings_pending = true;
  portEXIT_CRITICAL(&g_lock);
  return SettingsResult::Saved;
}

// ---- Arduino entry points ------------------------------------------------------
void setup() {
#ifdef SERIAL_DEBUG
  Serial.begin(115200);
#endif
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);

  // Settings and CAN first: the bridge must be forwarding before Wi-Fi starts.
  settings_load(&g_settings);
  leaf_inverter_config_t cfg;
  bridge_settings_to_inverter_config(&g_settings, &cfg);
  leaf_inverter_init(&g_inverter, &cfg);

  const bool can_ok = can_bus_begin();
  bridge_log("[bridge] firmware %s, leaf_can %s, CAN %s\n", FIRMWARE_VERSION, LEAF_CAN_VERSION_STRING,
               can_ok ? "OK" : "FAILED");
  (void)can_ok;
  publish_status();

  web_ui_begin();

  // Reboot instead of hanging if loop() ever stops running for 5 s.
  enableLoopWDT();
}

void loop() {
  leaf_can_frame_t frame;
  for (int i = 0; i < kMaxFramesPerPass && can_bus_receive(LEAF_BUS_VCM, &frame); i++) {
    bridge_frame(LEAF_BUS_VCM, frame);
  }
  for (int i = 0; i < kMaxFramesPerPass && can_bus_receive(LEAF_BUS_INVERTER, &frame); i++) {
    bridge_frame(LEAF_BUS_INVERTER, frame);
  }

  apply_pending_settings();

  const uint32_t now = millis();
  static uint32_t last_100ms = 0;
  if (now - last_100ms >= 100) {
    last_100ms = now;
    can_bus_service();
    publish_status();
  }

  static uint32_t last_led = 0;
  const CanBusStats can = can_bus_stats();
  const bool healthy = can.running[LEAF_BUS_VCM] && can.running[LEAF_BUS_INVERTER];
  if (now - last_led >= (healthy ? 500U : 100U)) {
    last_led = now;
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
  }

  web_ui_loop();
}
