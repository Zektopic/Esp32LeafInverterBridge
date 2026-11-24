/*
 * Host unit tests for bridge_settings.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_settings.h"

static int g_failures, g_checks;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static bool parse(const char *json, const bridge_settings_t *cur, bridge_settings_t *out, char *err)
{
    return bridge_settings_parse_json(json, strlen(json), cur, out, err, 96);
}

int main(void)
{
    bridge_settings_t def, s, out;
    char err[96];
    char id[32];
    bridge_settings_defaults(&def);

    /* Exact body produced by index.html (JSON.stringify(..., null, 2)). */
    const char *ui = "{\n  \"Vehicle\": \"AZE0\",\n  \"Inverter\": \"160\",\n  \"Battery\": \"62\",\n"
                     "  \"BatterySaver\": \"0\",\n  \"Glide\": \"0\",\n  \"Capacity\": \"1\",\n"
                     "  \"CurrentControl\": \"-1\"\n}";
    CHECK(parse(ui, &def, &s, err));
    CHECK(s.value[BRIDGE_SETTING_VEHICLE] == BRIDGE_VEHICLE_AZE0_2013_2017);
    CHECK(s.value[BRIDGE_SETTING_INVERTER] == BRIDGE_INVERTER_160KW);
    CHECK(s.value[BRIDGE_SETTING_BATTERY] == 3);
    CHECK(s.value[BRIDGE_SETTING_BATTERY_SAVER] == 3);
    CHECK(s.value[BRIDGE_SETTING_GLIDE_IN_DRIVE] == 1);
    CHECK(s.value[BRIDGE_SETTING_CAPACITY_BOOST] == 0);
    CHECK(s.value[BRIDGE_SETTING_CURRENT_CONTROL] == 5);

    /* Radio ids sent back over the WebSocket. */
    CHECK(bridge_settings_radio_id(&s, BRIDGE_SETTING_VEHICLE, id, sizeof(id)) && strcmp(id, "Vehicle4") == 0);
    CHECK(bridge_settings_radio_id(&s, BRIDGE_SETTING_INVERTER, id, sizeof(id)) && strcmp(id, "Inverter2") == 0);
    CHECK(bridge_settings_radio_id(&s, BRIDGE_SETTING_CURRENT_CONTROL, id, sizeof(id)) &&
          strcmp(id, "CurrentControl6") == 0);
    CHECK(!bridge_settings_radio_id(&s, BRIDGE_SETTING_CURRENT_CONTROL, id, 5)); /* too small */

    /* Partial update keeps the other values; unknown keys and numbers are accepted. */
    CHECK(parse("{\"Inverter\": 0, \"Foo\": \"bar\"}", &s, &out, err));
    CHECK(out.value[BRIDGE_SETTING_INVERTER] == BRIDGE_INVERTER_DISABLED);
    CHECK(out.value[BRIDGE_SETTING_VEHICLE] == BRIDGE_VEHICLE_AZE0_2013_2017);
    CHECK(parse("{}", &s, &out, err) && bridge_settings_equal(&s, &out));

    /* Rejections leave *out untouched and explain why. */
    out = def;
    CHECK(!parse("{\"Inverter\":\"999\"}", &s, &out, err));
    CHECK(strstr(err, "Inverter") != NULL);
    CHECK(bridge_settings_equal(&out, &def));
    CHECK(!parse("", &s, &out, err));
    CHECK(!parse("Vehicle=LEAF", &s, &out, err));
    CHECK(!parse("{\"Vehicle\":\"LEAF\"", &s, &out, err));           /* unterminated */
    CHECK(!parse("{\"Vehicle\":\"LEAF\"} x", &s, &out, err));        /* trailing */
    CHECK(!parse("{\"Vehicle\" \"LEAF\"}", &s, &out, err));          /* missing colon */
    CHECK(!parse("{\"Vehicle\":\"LEAF\",}", &s, &out, err));         /* trailing comma */
    CHECK(!parse("{\"Vehicle\":{\"a\":1}}", &s, &out, err));         /* nested */

    /* The v1.3.0 parser wrote past a String[14] array on long input; this must just fail cleanly. */
    char big[4096];
    size_t n = 0;
    n += (size_t)snprintf(big + n, sizeof(big) - n, "{");
    for (int i = 0; i < 200 && n < sizeof(big) - 32; i++) {
        n += (size_t)snprintf(big + n, sizeof(big) - n, "\"k%d\":\"v\",", i);
    }
    snprintf(big + n, sizeof(big) - n, "\"Vehicle\":\"ZE1\"}");
    CHECK(parse(big, &def, &out, err));
    CHECK(out.value[BRIDGE_SETTING_VEHICLE] == BRIDGE_VEHICLE_ZE1_2018_2022);
    memset(big, 'A', sizeof(big) - 1);
    big[0] = '{';
    big[1] = '"';
    big[sizeof(big) - 1] = '\0';
    CHECK(!parse(big, &def, &out, err)); /* over-long key */

    /* NVS sanitising and inverter mapping. */
    s = def;
    s.value[BRIDGE_SETTING_INVERTER] = 77;
    bridge_settings_sanitize(&s);
    CHECK(s.value[BRIDGE_SETTING_INVERTER] == BRIDGE_INVERTER_110KW);

    leaf_inverter_config_t cfg;
    s.value[BRIDGE_SETTING_INVERTER] = BRIDGE_INVERTER_160KW;
    bridge_settings_to_inverter_config(&s, &cfg);
    CHECK(cfg.modify_torque && cfg.drive_torque_permille == LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE);
    s.value[BRIDGE_SETTING_INVERTER] = BRIDGE_INVERTER_DISABLED;
    bridge_settings_to_inverter_config(&s, &cfg);
    CHECK(!cfg.modify_torque);

    /* v1.3.0 NVS keys must not change. */
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_VEHICLE), "VehSelect") == 0);
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_INVERTER), "InvUpgrade") == 0);
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_BATTERY), "BattSelect") == 0);
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_BATTERY_SAVER), "BattSaver") == 0);
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_GLIDE_IN_DRIVE), "GlideDrive") == 0);
    CHECK(strcmp(bridge_setting_nvs_key(BRIDGE_SETTING_CURRENT_CONTROL), "CurrCont") == 0);
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        CHECK(strlen(bridge_setting_nvs_key((bridge_setting_id_t)i)) <= 15); /* NVS key limit */
        CHECK(bridge_setting_option_count((bridge_setting_id_t)i) > 0);
    }
    CHECK(bridge_setting_is_implemented(BRIDGE_SETTING_INVERTER));
    CHECK(!bridge_setting_is_implemented(BRIDGE_SETTING_BATTERY_SAVER));

    printf("bridge_settings unit tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
