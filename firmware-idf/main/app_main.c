/*
 * Esp32LeafInverterBridge - ESP-IDF firmware
 *
 * Two-bus CAN bridge between the VCM side of a Nissan LEAF / e-NV200 EV-CAN and a
 * 110 kW / 160 kW (EM57, 2018+) inverter. The message handling is the shared,
 * hardware independent lib/leaf_can library; this firmware provides the drivers
 * (on-chip TWAI + MCP2515), the bridge task, NVS settings and the web UI.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "settings_store.h"
#include "web_server.h"
#include "network.h"

static const char *TAG = "main";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing (%s), settings reset to defaults", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly flashed OTA image is only
 * kept if it confirms itself; otherwise the next reset boots the previous one. */
static void confirm_image(bool healthy)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (healthy) {
        ESP_LOGI(TAG, "new firmware is healthy, cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "new firmware failed its self test, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Esp32LeafInverterBridge %s (ESP-IDF %s)", app->version, app->idf_ver);

    init_nvs();
    bridge_settings_t settings;
    if (settings_store_load(&settings) != ESP_OK) {
        bridge_settings_defaults(&settings);
    }

    /* CAN first: the bridge must be forwarding before Wi-Fi comes up. */
    const esp_err_t can_err = bridge_start(&settings);
    ESP_ERROR_CHECK(network_start());
    ESP_ERROR_CHECK(web_server_start());
#if CONFIG_BRIDGE_SELFTEST_REQUIRE_CAN
    confirm_image(can_err == ESP_OK);
#else
    (void)can_err;
    confirm_image(true);
#endif

#if CONFIG_BRIDGE_STATUS_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_BRIDGE_STATUS_LED_GPIO);
    gpio_set_direction(CONFIG_BRIDGE_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    int level = 0;
    for (;;) {
        /* slow blink: both CAN controllers running; fast blink: a controller failed */
        level = !level;
        gpio_set_level(CONFIG_BRIDGE_STATUS_LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(bridge_buses_running() ? 500 : 100));
    }
#endif
}
