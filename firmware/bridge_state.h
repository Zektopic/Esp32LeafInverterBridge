//——————————————————————————————————————————————————————————————————————————————
// bridge_state.h - thread-safe view of the bridge for the web server task.
// SPDX-License-Identifier: GPL-3.0-only
//
// The bridge (leaf_inverter_t) is owned by loop(). The async web server runs in
// another FreeRTOS task, so it only ever sees snapshots and hands settings
// changes over through bridge_request_settings().
//——————————————————————————————————————————————————————————————————————————————
#ifndef BRIDGE_STATE_H
#define BRIDGE_STATE_H

#include <bridge_settings.h>
#include <leaf_can.h>

#include "can_bus.h"

struct BridgeStatus {
  uint32_t uptime_s;
  leaf_shifter_t shifter;
  int16_t torque_request_vcm;   // 0.25 Nm, as sent by the VCM
  int16_t torque_request_out;   // 0.25 Nm, as forwarded to the inverter
  int16_t torque_response;      // 0.5 Nm, as reported by the inverter
  uint8_t charge_status;        // 0x1F2 data[2]
  bool ac_charging;
  bool soc_valid;
  uint16_t soc_permille;
  leaf_inverter_stats_t inverter;
  CanBusStats can;
};

enum class SettingsResult {
  Saved,
  Unchanged,
  NotParked,   // refused: car not in P
  StorageError,
};

BridgeStatus bridge_status();
bridge_settings_t bridge_settings();
bool bridge_is_parked();

// Validate state, persist and apply new settings. Safe to call from any task.
SettingsResult bridge_request_settings(const bridge_settings_t &settings);

#endif // BRIDGE_STATE_H
