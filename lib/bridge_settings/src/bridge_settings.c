/*
 * bridge_settings.c - user settings of the CAN bridge.
 *
 * Part of Esp32LeafInverterBridge.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "bridge_settings.h"

#include <stdio.h>
#include <string.h>

#define MAX_OPTIONS 6
#define MAX_TOKEN   32

typedef struct {
    const char *form_name;
    const char *nvs_key;
    bool implemented;
    uint8_t default_option;
    uint8_t option_count;
    const char *values[MAX_OPTIONS];
    const char *labels[MAX_OPTIONS];
} setting_desc_t;

/* Order of options == order of the radio buttons in index.html == v1.3.0 enum values. */
static const setting_desc_t k_settings[BRIDGE_SETTING_COUNT] = {
    [BRIDGE_SETTING_VEHICLE] = {"Vehicle", "VehSelect", false, 0, 5,
                                {"LEAF", "ENV200", "ZE0", "AZE0", "ZE1"},
                                {"Nissan LEAF 2010-2019", "Nissan e-NV200", "LEAF ZE0 2011-2012",
                                 "LEAF AZE0 2013-2017", "LEAF ZE1 2018-2022"}},
    [BRIDGE_SETTING_INVERTER] = {"Inverter", "InvUpgrade", true, 0, 3,
                                 {"110", "160", "0"},
                                 {"EM57 motor with 110 kW inverter", "EM57 motor with 160 kW inverter",
                                  "Disabled"}},
    [BRIDGE_SETTING_BATTERY] = {"Battery", "BattSelect", false, 0, 5,
                                {"24", "30", "40", "62", "-1"},
                                {"24 kWh", "30 kWh", "40 kWh", "62 kWh", "Brute force 30 kWh on 24 kWh LBC"}},
    [BRIDGE_SETTING_BATTERY_SAVER] = {"BatterySaver", "BattSaver", false, 0, 4,
                                      {"50", "60", "80", "0"},
                                      {"50 %", "60 %", "80 %", "Disabled"}},
    [BRIDGE_SETTING_GLIDE_IN_DRIVE] = {"Glide", "GlideDrive", false, 0, 2,
                                       {"1", "0"},
                                       {"Enabled", "Disabled"}},
    [BRIDGE_SETTING_CAPACITY_BOOST] = {"Capacity", "CapBoost", false, 0, 2,
                                       {"1", "0"},
                                       {"Enabled", "Disabled"}},
    [BRIDGE_SETTING_CURRENT_CONTROL] = {"CurrentControl", "CurrCont", false, 0, 6,
                                        {"1", "2", "3", "4", "6", "-1"},
                                        {"1.0 kW", "2.0 kW", "3.0 kW", "4.0 kW", "6.0 kW", "Unrestricted"}},
};

static const setting_desc_t *desc(bridge_setting_id_t id)
{
    return ((unsigned)id < (unsigned)BRIDGE_SETTING_COUNT) ? &k_settings[id] : NULL;
}

const char *bridge_setting_form_name(bridge_setting_id_t id)
{
    const setting_desc_t *d = desc(id);
    return d ? d->form_name : "";
}

const char *bridge_setting_nvs_key(bridge_setting_id_t id)
{
    const setting_desc_t *d = desc(id);
    return d ? d->nvs_key : "";
}

uint8_t bridge_setting_option_count(bridge_setting_id_t id)
{
    const setting_desc_t *d = desc(id);
    return d ? d->option_count : 0;
}

const char *bridge_setting_option_value(bridge_setting_id_t id, uint8_t option)
{
    const setting_desc_t *d = desc(id);
    return (d && option < d->option_count) ? d->values[option] : "";
}

const char *bridge_setting_option_label(bridge_setting_id_t id, uint8_t option)
{
    const setting_desc_t *d = desc(id);
    return (d && option < d->option_count) ? d->labels[option] : "";
}

bool bridge_setting_is_implemented(bridge_setting_id_t id)
{
    const setting_desc_t *d = desc(id);
    return d ? d->implemented : false;
}

void bridge_settings_defaults(bridge_settings_t *s)
{
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        s->value[i] = k_settings[i].default_option;
    }
}

void bridge_settings_sanitize(bridge_settings_t *s)
{
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        if (s->value[i] >= k_settings[i].option_count) {
            s->value[i] = k_settings[i].default_option;
        }
    }
}

bool bridge_settings_equal(const bridge_settings_t *a, const bridge_settings_t *b)
{
    return memcmp(a->value, b->value, sizeof(a->value)) == 0;
}

bool bridge_settings_radio_id(const bridge_settings_t *s, bridge_setting_id_t id, char *buf, size_t len)
{
    const setting_desc_t *d = desc(id);
    if (d == NULL || buf == NULL || len == 0) {
        return false;
    }
    const uint8_t option = (s->value[id] < d->option_count) ? s->value[id] : d->default_option;
    const int n = snprintf(buf, len, "%s%u", d->form_name, (unsigned)option + 1u);
    return n > 0 && (size_t)n < len;
}

/* ---- minimal JSON object parser (flat object of strings / numbers) ------------- */
typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static bool parse_string(cursor_t *c, char *out, size_t out_len)
{
    size_t n = 0;
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if ((unsigned char)ch < 0x20u) {
            return false;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                return false;
            }
            ch = *c->p++;
            if (ch != '"' && ch != '\\' && ch != '/') {
                return false; /* no other escapes are needed for these settings */
            }
        }
        if (n + 1 >= out_len) {
            return false;
        }
        out[n++] = ch;
    }
    if (c->p >= c->end) {
        return false;
    }
    c->p++; /* closing quote */
    out[n] = '\0';
    return true;
}

/* Bare number or literal (true/false/null): copy until a delimiter. */
static bool parse_bare(cursor_t *c, char *out, size_t out_len)
{
    size_t n = 0;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ' ' && *c->p != '\t' && *c->p != '\r' &&
           *c->p != '\n') {
        const char ch = *c->p;
        if (!((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || (ch >= 'a' && ch <= 'z'))) {
            return false;
        }
        if (n + 1 >= out_len) {
            return false;
        }
        out[n++] = ch;
        c->p++;
    }
    out[n] = '\0';
    return n > 0;
}

static void set_err(char *err, size_t err_len, const char *fmt, const char *a, const char *b)
{
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, fmt, a, b);
    }
}

bool bridge_settings_parse_json(const char *body, size_t len, const bridge_settings_t *current,
                                bridge_settings_t *out, char *err, size_t err_len)
{
    cursor_t c = {body, body + len};
    bridge_settings_t next = *current;
    char key[MAX_TOKEN];
    char value[MAX_TOKEN];

    if (body == NULL) {
        set_err(err, err_len, "%s%s", "empty request", "");
        return false;
    }
    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') {
        set_err(err, err_len, "%s%s", "expected a JSON object", "");
        return false;
    }
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
    } else {
        for (;;) {
            skip_ws(&c);
            if (!parse_string(&c, key, sizeof(key))) {
                set_err(err, err_len, "%s%s", "malformed or too long key", "");
                return false;
            }
            skip_ws(&c);
            if (c.p >= c.end || *c.p != ':') {
                set_err(err, err_len, "missing ':' after \"%s\"%s", key, "");
                return false;
            }
            c.p++;
            skip_ws(&c);
            const bool ok = (c.p < c.end && *c.p == '"') ? parse_string(&c, value, sizeof(value))
                                                        : parse_bare(&c, value, sizeof(value));
            if (!ok) {
                set_err(err, err_len, "malformed or too long value for \"%s\"%s", key, "");
                return false;
            }

            for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
                if (strcmp(key, k_settings[i].form_name) != 0) {
                    continue;
                }
                int found = -1;
                for (int o = 0; o < k_settings[i].option_count; o++) {
                    if (strcmp(value, k_settings[i].values[o]) == 0) {
                        found = o;
                        break;
                    }
                }
                if (found < 0) {
                    set_err(err, err_len, "invalid value \"%s\" for %s", value, key);
                    return false;
                }
                next.value[i] = (uint8_t)found;
                break;
            }

            skip_ws(&c);
            if (c.p < c.end && *c.p == ',') {
                c.p++;
                continue;
            }
            if (c.p < c.end && *c.p == '}') {
                c.p++;
                break;
            }
            set_err(err, err_len, "%s%s", "expected ',' or '}'", "");
            return false;
        }
    }
    skip_ws(&c);
    if (c.p != c.end) {
        set_err(err, err_len, "%s%s", "trailing data after JSON object", "");
        return false;
    }
    *out = next;
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return true;
}

void bridge_settings_to_inverter_config(const bridge_settings_t *s, leaf_inverter_config_t *cfg)
{
    uint8_t inverter = s->value[BRIDGE_SETTING_INVERTER];
    if (inverter >= k_settings[BRIDGE_SETTING_INVERTER].option_count) {
        inverter = k_settings[BRIDGE_SETTING_INVERTER].default_option;
    }
    leaf_inverter_default_config(cfg, (leaf_inverter_upgrade_t)inverter);
}
