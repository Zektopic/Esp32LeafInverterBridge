/*
 * sysinfo.c - device diagnostics for GET /api/system.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "sysinfo.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "captive_dns.h"
#include "mcp2515.h"
#include "network.h"
#include "security.h"

const char *sysinfo_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software restart";
    case ESP_RST_PANIC:
        return "crash (panic)";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep wake-up";
    case ESP_RST_BROWNOUT:
        return "brown-out (supply voltage dropped)";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "unknown";
    }
}

bool sysinfo_coredump_present(size_t *size)
{
    size_t addr = 0, len = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &len) != ESP_OK || len == 0) {
        return false;
    }
    if (size != NULL) {
        *size = len;
    }
    return true;
}

static void write_task(json_writer_t *w, const char *name, uint32_t stack_free)
{
    jw_obj_begin(w);
    jw_kv_str(w, "name", name);
    jw_kv_uint(w, "stack_free", stack_free);
    jw_obj_end(w);
}

void sysinfo_write_json(json_writer_t *w)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);
    char elf_sha[17];
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    char revision[8];
    snprintf(revision, sizeof(revision), "v%d.%d", chip.revision / 100, chip.revision % 100);

    jw_obj_begin(w);

    jw_key(w, "device");
    jw_obj_begin(w);
    jw_kv_str(w, "model", CONFIG_IDF_TARGET);
    jw_kv_str(w, "revision", revision);
    jw_kv_uint(w, "cores", chip.cores);
    jw_kv_uint(w, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    jw_kv_uint(w, "flash_bytes", flash_size);
    jw_kv_str(w, "mac", mac_str);
    jw_kv_uint(w, "uptime_s", (unsigned long long)(esp_timer_get_time() / 1000000));
    jw_kv_str(w, "reset_reason", sysinfo_reset_reason());
    jw_obj_end(w);

    jw_key(w, "firmware");
    jw_obj_begin(w);
    jw_kv_str(w, "project", app->project_name);
    jw_kv_str(w, "version", app->version);
    jw_kv_str(w, "idf", app->idf_ver);
    jw_kv_str(w, "built", built);
    jw_kv_str(w, "elf_sha256", elf_sha);
    jw_obj_end(w);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t other;
    jw_key(w, "ota");
    jw_obj_begin(w);
    jw_kv_str(w, "running", running ? running->label : NULL);
    jw_kv_str(w, "next", next ? next->label : NULL);
    jw_kv_uint(w, "slot_bytes", next ? next->size : 0);
    jw_key(w, "next_version");
    if (next != NULL && esp_ota_get_partition_description(next, &other) == ESP_OK) {
        jw_str(w, other.version);
    } else {
        jw_null(w);
    }
    jw_obj_end(w);

    jw_key(w, "memory");
    jw_obj_begin(w);
    jw_kv_uint(w, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    jw_kv_uint(w, "heap_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    jw_kv_uint(w, "heap_largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    jw_obj_end(w);

    jw_key(w, "tasks");
    jw_arr_begin(w);
    write_task(w, "bridge", bridge_task_stack_free());
    write_task(w, "mcp2515", mcp2515_task_stack_free());
    static const char *const others[] = {"httpd", "dns", "tiT", "wifi", "main"};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(others[i]);
        if (t != NULL) {
            write_task(w, others[i], (uint32_t)uxTaskGetStackHighWaterMark(t));
        }
    }
    jw_arr_end(w);

    char ssid[SECURITY_WIFI_SSID_MAX + 1];
    security_get_wifi(ssid, sizeof(ssid), NULL, 0);
    uint8_t ip[4] = {0};
    char ip_str[16] = "";
    if (network_get_ip(ip)) {
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    jw_key(w, "network");
    jw_obj_begin(w);
    jw_kv_str(w, "ssid", ssid);
    jw_kv_str(w, "ip", ip_str);
    jw_kv_int(w, "channel", network_channel());
    jw_kv_int(w, "clients", network_client_count());
    jw_kv_uint(w, "dns_queries", captive_dns_queries());
    jw_obj_end(w);

    size_t dump_size = 0;
    const bool dump = sysinfo_coredump_present(&dump_size);
    jw_key(w, "coredump");
    jw_obj_begin(w);
    jw_kv_bool(w, "present", dump);
    jw_kv_uint(w, "bytes", dump ? dump_size : 0);
    jw_obj_end(w);

    jw_obj_end(w);
}
