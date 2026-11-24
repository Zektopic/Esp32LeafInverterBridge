//——————————————————————————————————————————————————————————————————————————————
// settings_store.cpp - bridge settings in NVS via Preferences.
// SPDX-License-Identifier: GPL-3.0-only
//——————————————————————————————————————————————————————————————————————————————
#include <Arduino.h>
#include <Preferences.h>

#include "settings_store.h"

void settings_load(bridge_settings_t *settings) {
  bridge_settings_defaults(settings);
  Preferences prefs;
  if (!prefs.begin(BRIDGE_SETTINGS_NVS_NAMESPACE, true)) {
    return; // first boot: namespace does not exist yet
  }
  for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
    const bridge_setting_id_t id = static_cast<bridge_setting_id_t>(i);
    const uint32_t v = prefs.getUInt(bridge_setting_nvs_key(id), settings->value[i]);
    settings->value[i] = v > 0xFF ? 0xFF : static_cast<uint8_t>(v);
  }
  prefs.end();
  bridge_settings_sanitize(settings);
}

bool settings_save(const bridge_settings_t *settings) {
  Preferences prefs;
  if (!prefs.begin(BRIDGE_SETTINGS_NVS_NAMESPACE, false)) {
    return false;
  }
  bool ok = true;
  for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
    const char *key = bridge_setting_nvs_key(static_cast<bridge_setting_id_t>(i));
    const uint32_t v = settings->value[i];
    if (!prefs.isKey(key) || prefs.getUInt(key, 0) != v) {
      if (prefs.putUInt(key, v) != sizeof(uint32_t)) {
        ok = false;
      }
    }
  }
  prefs.end();
  return ok;
}
