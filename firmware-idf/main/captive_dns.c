/*
 * captive_dns.c - DNS server of the captive portal (UDP port 53).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dns_reply.h"
#include "network.h"

static const char *TAG = "dns";

#define DNS_PORT       53
#define DNS_MAX_PACKET 512
#define TASK_STACK     3072
#define TASK_PRIO      2
#define TASK_CORE      0 /* with Wi-Fi and the web server; core 1 is reserved for CAN */

static volatile uint32_t s_queries;

static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t rx[DNS_MAX_PACKET];
    static uint8_t tx[DNS_MAX_PACKET];

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind to port %d failed", DNS_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive portal DNS listening on port %d", DNS_PORT);

    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint8_t ip[4];
        if (!network_get_ip(ip)) {
            continue;
        }
        const size_t len = dns_build_reply(rx, (size_t)n, ip, tx, sizeof(tx));
        if (len > 0) {
            (void)sendto(sock, tx, len, 0, (struct sockaddr *)&from, from_len);
            s_queries++;
        }
    }
}

esp_err_t captive_dns_start(void)
{
    if (xTaskCreatePinnedToCore(dns_task, "dns", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t captive_dns_queries(void)
{
    return s_queries;
}
