/*
 * network.c - Wi-Fi access point (or QEMU OpenCores Ethernet for tests).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "network.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if CONFIG_BRIDGE_QEMU_OPENETH
#include "esp_eth.h"
#endif

static const char *TAG = "net";

_Static_assert(sizeof(CONFIG_BRIDGE_WIFI_PASSWORD) - 1 >= 8, "Wi-Fi password must have at least 8 characters");

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " joined", MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " left", MAC2STR(e->mac));
    }
}

#if CONFIG_BRIDGE_QEMU_OPENETH
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "QEMU ethernet got ip " IPSTR, IP2STR(&e->ip_info.ip));
}

static esp_err_t qemu_eth_start(void)
{
    const esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    const esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_LOGW(TAG, "TEST BUILD: QEMU OpenCores Ethernet instead of Wi-Fi");
    return esp_eth_start(eth);
}
#endif

esp_err_t network_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_BRIDGE_QEMU_OPENETH
    return qemu_eth_start();
#endif
    (void)esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    wifi_config_t cfg = {
        .ap = {
            .channel = CONFIG_BRIDGE_WIFI_CHANNEL,
            .max_connection = CONFIG_BRIDGE_WIFI_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    strlcpy((char *)cfg.ap.ssid, CONFIG_BRIDGE_WIFI_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(CONFIG_BRIDGE_WIFI_SSID);
    strlcpy((char *)cfg.ap.password, CONFIG_BRIDGE_WIFI_PASSWORD, sizeof(cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "access point \"%s\" on channel %d, http://192.168.4.1/", CONFIG_BRIDGE_WIFI_SSID,
             CONFIG_BRIDGE_WIFI_CHANNEL);
    return ESP_OK;
}
