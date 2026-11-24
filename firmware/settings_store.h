//——————————————————————————————————————————————————————————————————————————————
// settings_store.h - bridge settings in NVS (same namespace / keys as v1.x).
// SPDX-License-Identifier: GPL-3.0-only
//——————————————————————————————————————————————————————————————————————————————
#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <bridge_settings.h>

// Load settings; missing or invalid values fall back to the defaults.
void settings_load(bridge_settings_t *settings);

// Write only the values that changed. Returns false on a flash error.
bool settings_save(const bridge_settings_t *settings);

#endif // SETTINGS_STORE_H
