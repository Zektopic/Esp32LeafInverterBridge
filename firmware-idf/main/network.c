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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if CONFIG_BRIDGE_QEMU_OPENETH
#include "esp_eth.h"
#endif

#include "security.h"

static const char *TAG = "net";

static esp_netif_t *s_netif;
static volatile int s_clients;
static esp_timer_handle_t s_apply_timer;

#if !CONFIG_BRIDGE_QEMU_OPENETH
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        s_clients++;
        ESP_LOGI(TAG, "client " MACSTR " joined (%d connected)", MAC2STR(e->mac), s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        if (s_clients > 0) {
            s_clients--;
        }
        ESP_LOGI(TAG, "client " MACSTR " left (%d connected)", MAC2STR(e->mac), s_clients);
    }
}
#endif

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
    s_netif = esp_netif_new(&netif_cfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    const esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_LOGW(TAG, "TEST BUILD: QEMU OpenCores Ethernet instead of Wi-Fi");
    return esp_eth_start(eth);
}
#endif

#if !CONFIG_BRIDGE_QEMU_OPENETH
static void fill_ap_config(wifi_config_t *cfg)
{
    char ssid[SECURITY_WIFI_SSID_MAX + 1];
    char password[SECURITY_WIFI_PASSWORD_MAX + 1];
    security_get_wifi(ssid, sizeof(ssid), password, sizeof(password));

    memset(cfg, 0, sizeof(*cfg));
    cfg->ap.channel = CONFIG_BRIDGE_WIFI_CHANNEL;
    cfg->ap.max_connection = CONFIG_BRIDGE_WIFI_MAX_CONNECTIONS;
    cfg->ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    cfg->ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    cfg->ap.pmf_cfg.capable = true;
    cfg->ap.pmf_cfg.required = false;
    memcpy(cfg->ap.ssid, ssid, strlen(ssid));
    cfg->ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(cfg->ap.password, password, strlen(password));
    memset(password, 0, sizeof(password));
}
#endif

static void apply_credentials(void *arg)
{
    (void)arg;
#if !CONFIG_BRIDGE_QEMU_OPENETH
    wifi_config_t cfg;
    fill_ap_config(&cfg);
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err == ESP_OK) {
        s_clients = 0;
        ESP_LOGI(TAG, "access point now \"%s\"", (const char *)cfg.ap.ssid);
    } else {
        ESP_LOGE(TAG, "could not apply the new access point settings: %s", esp_err_to_name(err));
    }
    memset(&cfg, 0, sizeof(cfg));
#else
    ESP_LOGI(TAG, "QEMU build: new access point settings stored (no Wi-Fi to apply them to)");
#endif
}

void network_apply_credentials_later(uint32_t delay_ms)
{
    if (s_apply_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = apply_credentials, .name = "wifi_apply"};
        if (esp_timer_create(&args, &s_apply_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_apply_timer);
    esp_timer_start_once(s_apply_timer, (uint64_t)delay_ms * 1000u);
}

esp_err_t network_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_BRIDGE_QEMU_OPENETH
    return qemu_eth_start();
#else
    s_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false; /* credentials live in our own namespace */
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    wifi_config_t cfg;
    fill_ap_config(&cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "access point \"%s\" on channel %d, http://192.168.4.1/", (const char *)cfg.ap.ssid,
             CONFIG_BRIDGE_WIFI_CHANNEL);
    memset(&cfg, 0, sizeof(cfg));
    return ESP_OK;
#endif
}

bool network_get_ip(uint8_t ip[4])
{
    esp_netif_ip_info_t info;
    if (s_netif == NULL || esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    /* esp_ip4_addr_t is in network byte order */
    memcpy(ip, &info.ip.addr, 4);
    return true;
}

int network_client_count(void)
{
    return s_clients;
}

int network_channel(void)
{
#if CONFIG_BRIDGE_QEMU_OPENETH
    return 0;
#else
    return CONFIG_BRIDGE_WIFI_CHANNEL;
#endif
}
