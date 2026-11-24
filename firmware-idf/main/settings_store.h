/*
 * settings_store.h - bridge settings in NVS (namespace / keys compatible with firmware v1.x).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "esp_err.h"

#include "bridge_settings.h"

/* Load settings; missing or invalid values fall back to the defaults. */
esp_err_t settings_store_load(bridge_settings_t *settings);
/* Write the values that changed and commit. */
esp_err_t settings_store_save(const bridge_settings_t *settings);
