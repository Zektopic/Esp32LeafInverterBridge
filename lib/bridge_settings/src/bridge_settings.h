/*
 * bridge_settings.h - user settings of the CAN bridge (web UI <-> NVS <-> leaf_can).
 *
 * Part of Esp32LeafInverterBridge.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * One table describes every setting: its web form field name, its NVS key (kept
 * identical to firmware v1.3.0 so existing devices keep their configuration),
 * and its options in the order of the radio buttons in index.html. A setting's
 * stored value is the zero-based option index, exactly like v1.3.0.
 */
#ifndef BRIDGE_SETTINGS_H
#define BRIDGE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "leaf_inverter_upgrade.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_SETTINGS_NVS_NAMESPACE "ESP32" /* v1.3.0 Preferences namespace */

typedef enum {
    BRIDGE_SETTING_VEHICLE = 0,
    BRIDGE_SETTING_INVERTER,
    BRIDGE_SETTING_BATTERY,
    BRIDGE_SETTING_BATTERY_SAVER,
    BRIDGE_SETTING_GLIDE_IN_DRIVE,
    BRIDGE_SETTING_CAPACITY_BOOST,
    BRIDGE_SETTING_CURRENT_CONTROL,
    BRIDGE_SETTING_COUNT
} bridge_setting_id_t;

/* Option indices of BRIDGE_SETTING_VEHICLE */
enum {
    BRIDGE_VEHICLE_LEAF_2010_2019 = 0,
    BRIDGE_VEHICLE_ENV200 = 1,
    BRIDGE_VEHICLE_ZE0_2011_2012 = 2,
    BRIDGE_VEHICLE_AZE0_2013_2017 = 3,
    BRIDGE_VEHICLE_ZE1_2018_2022 = 4,
};

/* Option indices of BRIDGE_SETTING_INVERTER (same numbering as leaf_inverter_upgrade_t) */
enum {
    BRIDGE_INVERTER_110KW = LEAF_INVERTER_UPGRADE_110KW,
    BRIDGE_INVERTER_160KW = LEAF_INVERTER_UPGRADE_160KW,
    BRIDGE_INVERTER_DISABLED = LEAF_INVERTER_UPGRADE_DISABLED,
};

typedef struct {
    uint8_t value[BRIDGE_SETTING_COUNT]; /* option index per setting */
} bridge_settings_t;

/* Metadata ------------------------------------------------------------------ */
const char *bridge_setting_form_name(bridge_setting_id_t id);   /* "Vehicle" */
const char *bridge_setting_nvs_key(bridge_setting_id_t id);     /* "VehSelect" */
uint8_t bridge_setting_option_count(bridge_setting_id_t id);
const char *bridge_setting_option_value(bridge_setting_id_t id, uint8_t option); /* "LEAF" */
const char *bridge_setting_option_label(bridge_setting_id_t id, uint8_t option); /* "Nissan LEAF 2010-2019" */
/* False for settings the firmware stores but does not act on yet. */
bool bridge_setting_is_implemented(bridge_setting_id_t id);

/* Values ---------------------------------------------------------------------- */
void bridge_settings_defaults(bridge_settings_t *s);
/* Replace out-of-range values (e.g. from an old or corrupted NVS) by the default. */
void bridge_settings_sanitize(bridge_settings_t *s);
bool bridge_settings_equal(const bridge_settings_t *a, const bridge_settings_t *b);

/*
 * Web UI radio button id of the current value, e.g. "Vehicle1" or "CurrentControl6".
 * The UI's WebSocket handler checks the matching radio button. Returns false if buf is too small.
 */
bool bridge_settings_radio_id(const bridge_settings_t *s, bridge_setting_id_t id, char *buf, size_t len);

/*
 * Parse the JSON object posted by index.html, e.g.
 *   {"Vehicle":"LEAF","Inverter":"160","Battery":"24",...}
 * Starts from *current: fields that are absent keep their value, unknown keys are
 * ignored. Returns false (and leaves *out untouched) on malformed JSON or an
 * unknown option value; `err` receives a human readable reason.
 */
bool bridge_settings_parse_json(const char *body, size_t len, const bridge_settings_t *current,
                                bridge_settings_t *out, char *err, size_t err_len);

/* leaf_can configuration that corresponds to the settings. */
void bridge_settings_to_inverter_config(const bridge_settings_t *s, leaf_inverter_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_SETTINGS_H */
