/*
 * bridge.c - the CAN bridge task.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "bridge.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_twai.h"
#include "mcp2515.h"
#include "settings_store.h"

static const char *TAG = "bridge";

#define BRIDGE_QUEUE_DEPTH   128
#define BRIDGE_TASK_STACK    4096
#define BRIDGE_TASK_PRIO     (configMAX_PRIORITIES - 3)
#define BRIDGE_TASK_CORE     1 /* Wi-Fi and the web server run on core 0 */
#define STATUS_PERIOD_US     (100 * 1000)

typedef struct {
    leaf_bus_t bus;
    leaf_can_frame_t frame;
} bridge_item_t;

static QueueHandle_t s_queue;
static uint32_t s_queue_overflow; /* written from ISR and tasks: approximate counter */

/* Owned by the bridge task. */
static leaf_inverter_t s_inverter;

/* Shared with other tasks, guarded by s_lock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bridge_settings_t s_settings;
static bool s_settings_pending;
static bridge_status_t s_status;

/* ---- driver side ------------------------------------------------------------------ */
void IRAM_ATTR bridge_post_from_isr(leaf_bus_t bus, const leaf_can_frame_t *frame, BaseType_t *woken)
{
    bridge_item_t item;
    item.bus = bus;
    item.frame = *frame;
    if (xQueueSendFromISR(s_queue, &item, woken) != pdTRUE) {
        s_queue_overflow++;
    }
}

void bridge_post(leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    bridge_item_t item = {.bus = bus, .frame = *frame};
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_queue_overflow++;
    }
}

static bool bus_send(leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    return (bus == LEAF_BUS_VCM) ? can_twai_send(frame) : mcp2515_send(frame);
}

static void emit_frame(void *ctx, leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    (void)ctx;
    (void)bus_send(bus, frame);
}

/* ---- bridge task -------------------------------------------------------------------- */
static void apply_pending_settings(void)
{
    bridge_settings_t s;
    bool pending;
    taskENTER_CRITICAL(&s_lock);
    pending = s_settings_pending;
    s = s_settings;
    s_settings_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    if (pending) {
        leaf_inverter_config_t cfg;
        bridge_settings_to_inverter_config(&s, &cfg);
        leaf_inverter_set_config(&s_inverter, &cfg);
        ESP_LOGI(TAG, "settings applied: inverter %s",
                 bridge_setting_option_label(BRIDGE_SETTING_INVERTER, s.value[BRIDGE_SETTING_INVERTER]));
    }
}

static void publish_status(void)
{
    bridge_status_t st;
    memset(&st, 0, sizeof(st));
    st.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    st.shifter = s_inverter.shifter;
    st.torque_request_vcm = s_inverter.vcm_torque_request;
    st.torque_request_out = s_inverter.torque_request_out;
    st.torque_response = s_inverter.torque_response_in;
    st.charge_status = s_inverter.charge_status;
    st.ac_charging = leaf_inverter_ac_charging(&s_inverter);
    st.soc_valid = s_inverter.soc_valid;
    st.soc_permille = s_inverter.soc_permille;
    st.inverter = s_inverter.stats;
    can_twai_get_stats(&st.bus[LEAF_BUS_VCM]);
    mcp2515_get_stats(&st.bus[LEAF_BUS_INVERTER]);
    st.queue_overflow = s_queue_overflow;
    st.queue_high_water = BRIDGE_QUEUE_DEPTH - (uint32_t)uxQueueSpacesAvailable(s_queue);

    taskENTER_CRITICAL(&s_lock);
    if (st.queue_high_water < s_status.queue_high_water) {
        st.queue_high_water = s_status.queue_high_water;
    }
    s_status = st;
    taskEXIT_CRITICAL(&s_lock);
}

static void bridge_task(void *arg)
{
    (void)arg;
    bridge_item_t item;
    int64_t next_status = 0;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (leaf_inverter_process(&s_inverter, item.bus, &item.frame, emit_frame, NULL)) {
                (void)bus_send(leaf_bus_other(item.bus), &item.frame);
            }
        }
        apply_pending_settings();

        const int64_t now = esp_timer_get_time();
        if (now >= next_status) {
            next_status = now + STATUS_PERIOD_US;
            can_twai_service();
            publish_status();
        }
        esp_task_wdt_reset();
    }
}

/* ---- public API --------------------------------------------------------------------- */
esp_err_t bridge_start(const bridge_settings_t *settings)
{
    leaf_inverter_config_t cfg;

    s_queue = xQueueCreate(BRIDGE_QUEUE_DEPTH, sizeof(bridge_item_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_settings = *settings;
    bridge_settings_to_inverter_config(settings, &cfg);
    leaf_inverter_init(&s_inverter, &cfg);

    /* Bring the buses up before anything else; each failure is reported but the
     * other bus keeps running so the problem shows in /api/status. */
    const esp_err_t twai_err = can_twai_start();
    if (twai_err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI (VCM bus) start failed: %s", esp_err_to_name(twai_err));
    }
    const esp_err_t mcp_err = mcp2515_start();
    if (mcp_err != ESP_OK) {
        ESP_LOGE(TAG, "MCP2515 (inverter bus) start failed: %s", esp_err_to_name(mcp_err));
    }
    publish_status();

    if (xTaskCreatePinnedToCore(bridge_task, "bridge", BRIDGE_TASK_STACK, NULL, BRIDGE_TASK_PRIO, NULL,
                                BRIDGE_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "bridge running, leaf_can %s, inverter %s", LEAF_CAN_VERSION_STRING,
             bridge_setting_option_label(BRIDGE_SETTING_INVERTER, settings->value[BRIDGE_SETTING_INVERTER]));
    return (twai_err != ESP_OK) ? twai_err : mcp_err;
}

void bridge_get_status(bridge_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

void bridge_get_settings(bridge_settings_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_settings;
    taskEXIT_CRITICAL(&s_lock);
}

bool bridge_is_parked(void)
{
    bridge_status_t st;
    bridge_get_status(&st);
    return st.shifter == LEAF_SHIFTER_PARK;
}

bool bridge_buses_running(void)
{
    bridge_status_t st;
    bridge_get_status(&st);
    return st.bus[LEAF_BUS_VCM].running && st.bus[LEAF_BUS_INVERTER].running;
}

bridge_settings_result_t bridge_request_settings(const bridge_settings_t *settings)
{
    bridge_settings_t current;
    bridge_get_settings(&current);
    if (bridge_settings_equal(settings, &current)) {
        return BRIDGE_SETTINGS_UNCHANGED;
    }
    /* A flash write stalls the CPU cache and the new torque map applies at once:
     * only while parked. */
    if (!bridge_is_parked()) {
        return BRIDGE_SETTINGS_NOT_PARKED;
    }
    if (settings_store_save(settings) != ESP_OK) {
        return BRIDGE_SETTINGS_STORAGE_ERROR;
    }
    taskENTER_CRITICAL(&s_lock);
    s_settings = *settings;
    s_settings_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    return BRIDGE_SETTINGS_SAVED;
}
