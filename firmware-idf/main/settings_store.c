/*
 * settings_store.c - bridge settings in NVS.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * v1.x stored each setting with Arduino Preferences::putUInt(), i.e. as an NVS
 * u32 in namespace "ESP32". Reading and writing the same keys means a device
 * keeps its configuration when it moves between the Arduino and IDF firmwares.
 */
#include "settings_store.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

esp_err_t settings_store_load(bridge_settings_t *settings)
{
    nvs_handle_t h;
    bridge_settings_defaults(settings);
    esp_err_t err = nvs_open(BRIDGE_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        uint32_t v;
        if (nvs_get_u32(h, bridge_setting_nvs_key((bridge_setting_id_t)i), &v) == ESP_OK) {
            settings->value[i] = (v > 0xFF) ? 0xFF : (uint8_t)v;
        }
    }
    nvs_close(h);
    bridge_settings_sanitize(settings);
    return ESP_OK;
}

esp_err_t settings_store_save(const bridge_settings_t *settings)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIDGE_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < BRIDGE_SETTING_COUNT && err == ESP_OK; i++) {
        const char *key = bridge_setting_nvs_key((bridge_setting_id_t)i);
        uint32_t old;
        if (nvs_get_u32(h, key, &old) != ESP_OK || old != settings->value[i]) {
            err = nvs_set_u32(h, key, settings->value[i]);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
    }
    return err;
}
