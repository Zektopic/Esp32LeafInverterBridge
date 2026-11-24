/*
 * bridge.c - the CAN bridge task.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "bridge.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
#define RATE_PERIOD_US       (1000 * 1000)
#define START_TIMEOUT_MS     3000

typedef struct {
    leaf_bus_t bus;
    uint32_t t_cycles; /* CPU cycle counter when the frame was received (same core) */
    leaf_can_frame_t frame;
} bridge_item_t;

static QueueHandle_t s_queue;
static uint32_t s_queue_overflow; /* written from ISR and tasks: approximate counter */
static TaskHandle_t s_task;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err[LEAF_BUS_COUNT];

/* Owned by the bridge task. */
static leaf_inverter_t s_inverter;
static bridge_settings_t s_applied; /* settings s_inverter currently runs with */
static uint32_t s_ticks_per_us;
static uint32_t s_high_water;
static uint64_t s_lat_sum;
static uint32_t s_lat_count;
static uint32_t s_lat_max;
static uint32_t s_lat_peak;
static uint32_t s_lat_avg_pub;
static uint32_t s_lat_max_pub;
static int64_t s_rate_t0;
static uint32_t s_rate_rx0[LEAF_BUS_COUNT];
static uint32_t s_rate_tx0[LEAF_BUS_COUNT];
static uint32_t s_rate_rx[LEAF_BUS_COUNT];
static uint32_t s_rate_tx[LEAF_BUS_COUNT];

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
    item.t_cycles = (uint32_t)esp_cpu_get_cycle_count();
    item.frame = *frame;
    if (xQueueSendFromISR(s_queue, &item, woken) != pdTRUE) {
        s_queue_overflow++;
    }
}

void bridge_post(leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    bridge_item_t item = {.bus = bus, .t_cycles = (uint32_t)esp_cpu_get_cycle_count(), .frame = *frame};
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
        s_applied = s;
        ESP_LOGI(TAG, "settings applied: inverter %s",
                 bridge_setting_option_label(BRIDGE_SETTING_INVERTER, s.value[BRIDGE_SETTING_INVERTER]));
    }
}

static void record_latency(uint32_t t_cycles)
{
    const uint32_t cycles = (uint32_t)esp_cpu_get_cycle_count() - t_cycles;
    const uint32_t us = cycles / s_ticks_per_us;
    if (us > 1000000u) {
        return; /* counter of another core or a wrap: not a real measurement */
    }
    s_lat_sum += us;
    s_lat_count++;
    if (us > s_lat_max) {
        s_lat_max = us;
    }
    if (us > s_lat_peak) {
        s_lat_peak = us;
    }
}

static void update_rates(int64_t now, const bridge_status_t *st)
{
    if (now - s_rate_t0 < RATE_PERIOD_US) {
        return;
    }
    const uint64_t dt = (uint64_t)(now - s_rate_t0);
    for (int b = 0; b < LEAF_BUS_COUNT; b++) {
        s_rate_rx[b] = (uint32_t)(((uint64_t)(st->bus[b].rx - s_rate_rx0[b]) * 1000000u + dt / 2) / dt);
        s_rate_tx[b] = (uint32_t)(((uint64_t)(st->bus[b].tx - s_rate_tx0[b]) * 1000000u + dt / 2) / dt);
        s_rate_rx0[b] = st->bus[b].rx;
        s_rate_tx0[b] = st->bus[b].tx;
    }
    s_lat_avg_pub = s_lat_count ? (uint32_t)(s_lat_sum / s_lat_count) : 0u;
    s_lat_max_pub = s_lat_max;
    s_lat_sum = 0;
    s_lat_count = 0;
    s_lat_max = 0;
    s_rate_t0 = now;
}

static void publish_status(void)
{
    bridge_status_t st;
    const int64_t now = esp_timer_get_time();
    memset(&st, 0, sizeof(st));
    st.uptime_s = (uint32_t)(now / 1000000);
    st.shifter = s_inverter.shifter;
    st.torque_request_vcm = s_inverter.vcm_torque_request;
    st.torque_request_out = s_inverter.torque_request_out;
    st.torque_response = s_inverter.torque_response_in;
    st.charge_status = s_inverter.charge_status;
    st.ac_charging = leaf_inverter_ac_charging(&s_inverter);
    st.soc_valid = s_inverter.soc_valid;
    st.soc_permille = s_inverter.soc_permille;
    st.applied = s_applied;      /* published together with the config derived from it */
    st.config = s_inverter.cfg;
    st.inverter = s_inverter.stats;
    can_twai_get_stats(&st.bus[LEAF_BUS_VCM]);
    mcp2515_get_stats(&st.bus[LEAF_BUS_INVERTER]);
    update_rates(now, &st);
    for (int b = 0; b < LEAF_BUS_COUNT; b++) {
        st.bus[b].rx_per_s = s_rate_rx[b];
        st.bus[b].tx_per_s = s_rate_tx[b];
    }
    st.queue_overflow = s_queue_overflow;
    st.queue_high_water = s_high_water;
    st.queue_depth = BRIDGE_QUEUE_DEPTH;
    st.latency_us_avg = s_lat_avg_pub;
    st.latency_us_max = s_lat_max_pub;
    st.latency_us_peak = s_lat_peak;

    taskENTER_CRITICAL(&s_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_lock);
}

static void bridge_task(void *arg)
{
    (void)arg;
    bridge_item_t item;
    int64_t next_status = 0;

    /* Started here (CPU 1) so the TWAI and MCP2515 interrupts are served by CPU 1. */
    s_ticks_per_us = esp_rom_get_cpu_ticks_per_us();
    if (s_ticks_per_us == 0) {
        s_ticks_per_us = 240;
    }
    s_start_err[LEAF_BUS_VCM] = can_twai_start();
    if (s_start_err[LEAF_BUS_VCM] != ESP_OK) {
        ESP_LOGE(TAG, "TWAI (VCM bus) start failed: %s", esp_err_to_name(s_start_err[LEAF_BUS_VCM]));
    }
    s_start_err[LEAF_BUS_INVERTER] = mcp2515_start();
    if (s_start_err[LEAF_BUS_INVERTER] != ESP_OK) {
        ESP_LOGE(TAG, "MCP2515 (inverter bus) start failed: %s - retrying in the background",
                 esp_err_to_name(s_start_err[LEAF_BUS_INVERTER]));
    }
    s_rate_t0 = esp_timer_get_time();
    publish_status();
    xSemaphoreGive(s_started);

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(20)) == pdTRUE) {
            const uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(s_queue) + 1u;
            if (waiting > s_high_water) {
                s_high_water = waiting;
            }
            if (leaf_inverter_process(&s_inverter, item.bus, &item.frame, emit_frame, NULL)) {
                (void)bus_send(leaf_bus_other(item.bus), &item.frame);
            }
            record_latency(item.t_cycles);
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
    s_started = xSemaphoreCreateBinary();
    if (s_queue == NULL || s_started == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_settings = *settings;
    s_applied = *settings;
    bridge_settings_to_inverter_config(settings, &cfg);
    leaf_inverter_init(&s_inverter, &cfg);

    if (xTaskCreatePinnedToCore(bridge_task, "bridge", BRIDGE_TASK_STACK, NULL, BRIDGE_TASK_PRIO, &s_task,
                                BRIDGE_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_started, pdMS_TO_TICKS(START_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "CAN drivers did not start in time");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "bridge running on CPU %d, leaf_can %s, inverter %s", BRIDGE_TASK_CORE, LEAF_CAN_VERSION_STRING,
             bridge_setting_option_label(BRIDGE_SETTING_INVERTER, settings->value[BRIDGE_SETTING_INVERTER]));
    return (s_start_err[LEAF_BUS_VCM] != ESP_OK) ? s_start_err[LEAF_BUS_VCM] : s_start_err[LEAF_BUS_INVERTER];
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

const char *bridge_bus_state_name(uint8_t state)
{
    switch (state) {
    case BRIDGE_BUS_ACTIVE:
        return "active";
    case BRIDGE_BUS_WARNING:
        return "warning";
    case BRIDGE_BUS_PASSIVE:
        return "passive";
    case BRIDGE_BUS_OFF:
        return "bus_off";
    case BRIDGE_BUS_STOPPED:
    default:
        return "stopped";
    }
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

uint32_t bridge_task_stack_free(void)
{
    return (s_task != NULL) ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0u;
}
