/*
 * can_twai.c - VCM bus on the ESP32 on-chip TWAI controller.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The ESP-IDF 6 TWAI driver keeps a pointer to every frame passed to
 * twai_node_transmit() until its on_tx_done callback fires, so frames are sent
 * from a fixed pool of slots that the callback hands back through a queue.
 * All callbacks run in the ISR (from IRAM, see CONFIG_TWAI_ISR_CACHE_SAFE).
 * can_twai_start() is called from the bridge task on CPU 1, so the interrupt
 * is allocated on (and served by) CPU 1.
 */
#include "can_twai.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "twai";

#define TX_QUEUE_DEPTH 16
#define TX_SLOTS       (TX_QUEUE_DEPTH + 8)

/* 500 kbit/s exactly as firmware v1.x configured the controller: 80 MHz / 10 =
 * 125 ns quanta, 1 + 13 + 2 = 16 quanta, sample point 87.5 %, SJW 2. */
#define BITRATE        500000
#define TIMING_BRP     10
#define TIMING_TSEG1   13
#define TIMING_TSEG2   2
#define TIMING_SJW     2

typedef struct {
    twai_frame_t frame;
    uint8_t data[LEAF_CAN_MAX_DLC];
} tx_slot_t;

static twai_node_handle_t s_node;
static tx_slot_t s_slots[TX_SLOTS];
static QueueHandle_t s_free_slots; /* uint8_t slot indices */
static volatile bool s_bus_off;
static bool s_recovering;
static bridge_bus_stats_t s_stats;

static bool IRAM_ATTR on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    (void)edata;
    (void)ctx;
    uint8_t buf[LEAF_CAN_MAX_DLC];
    twai_frame_t rx = {.buffer = buf, .buffer_len = sizeof(buf)};
    BaseType_t woken = pdFALSE;

    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) {
        s_stats.rx_overflow++;
        return false;
    }
    leaf_can_frame_t f;
    f.id = rx.header.id;
    f.dlc = (rx.header.dlc > LEAF_CAN_MAX_DLC) ? LEAF_CAN_MAX_DLC : (uint8_t)rx.header.dlc;
    f.flags = (uint8_t)((rx.header.ide ? LEAF_CAN_FLAG_EXTENDED : 0u) | (rx.header.rtr ? LEAF_CAN_FLAG_RTR : 0u));
    for (unsigned i = 0; i < LEAF_CAN_MAX_DLC; i++) {
        f.data[i] = (i < f.dlc && !rx.header.rtr) ? buf[i] : 0u;
    }
    s_stats.rx++;
    bridge_post_from_isr(LEAF_BUS_VCM, &f, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *ctx)
{
    (void)node;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    const tx_slot_t *slot = (const tx_slot_t *)edata->done_tx_frame; /* frame is the first member */
    const uint8_t idx = (uint8_t)(slot - s_slots);
    if (edata->is_tx_success) {
        s_stats.tx++;
    } else {
        s_stats.tx_dropped++;
    }
    if (idx < TX_SLOTS) {
        (void)xQueueSendFromISR(s_free_slots, &idx, &woken);
    }
    return woken == pdTRUE;
}

static uint8_t IRAM_ATTR map_state(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE:
        return BRIDGE_BUS_ACTIVE;
    case TWAI_ERROR_WARNING:
        return BRIDGE_BUS_WARNING;
    case TWAI_ERROR_PASSIVE:
        return BRIDGE_BUS_PASSIVE;
    case TWAI_ERROR_BUS_OFF:
    default:
        return BRIDGE_BUS_OFF;
    }
}

static bool IRAM_ATTR on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t *edata,
                                      void *ctx)
{
    (void)node;
    (void)ctx;
    s_stats.state = map_state(edata->new_sta);
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = true;
        s_stats.bus_off++;
    } else if (edata->old_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = false;
    }
    return false;
}

static void refill_slots(void)
{
    xQueueReset(s_free_slots);
    for (uint8_t i = 0; i < TX_SLOTS; i++) {
        (void)xQueueSend(s_free_slots, &i, 0);
    }
}

esp_err_t can_twai_start(void)
{
    s_free_slots = xQueueCreate(TX_SLOTS, sizeof(uint8_t));
    if (s_free_slots == NULL) {
        return ESP_ERR_NO_MEM;
    }
    refill_slots();

    const twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = CONFIG_BRIDGE_TWAI_TX_GPIO,
            .rx = CONFIG_BRIDGE_TWAI_RX_GPIO,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {.bitrate = BITRATE},
        .fail_retry_cnt = -1, /* standard CAN: retransmit until acknowledged */
        .tx_queue_depth = TX_QUEUE_DEPTH,
        .intr_priority = 0,
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        return err;
    }

    const twai_timing_advanced_config_t timing = {
        .brp = TIMING_BRP,
        .prop_seg = 0,
        .tseg_1 = TIMING_TSEG1,
        .tseg_2 = TIMING_TSEG2,
        .sjw = TIMING_SJW,
        .ssp_offset = 0,
    };
    err = twai_node_reconfig_timing(s_node, &timing, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "exact v1.x bit timing rejected (%s), using driver default sample point",
                 esp_err_to_name(err));
    }

    const twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
    };
    err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        return err;
    }
    s_stats.running = true;
    s_stats.state = BRIDGE_BUS_ACTIVE;
    ESP_LOGI(TAG, "VCM bus running on TX GPIO%d / RX GPIO%d at %d bit/s", CONFIG_BRIDGE_TWAI_TX_GPIO,
             CONFIG_BRIDGE_TWAI_RX_GPIO, BITRATE);
    return ESP_OK;
}

bool can_twai_send(const leaf_can_frame_t *frame)
{
    uint8_t idx;
    if (!s_stats.running || s_bus_off || xQueueReceive(s_free_slots, &idx, 0) != pdTRUE) {
        s_stats.tx_dropped++;
        return false;
    }
    tx_slot_t *slot = &s_slots[idx];
    memset(&slot->frame, 0, sizeof(slot->frame));
    memcpy(slot->data, frame->data, LEAF_CAN_MAX_DLC);
    slot->frame.header.id = frame->id;
    slot->frame.header.ide = leaf_can_frame_is_extended(frame) ? 1u : 0u;
    slot->frame.header.rtr = leaf_can_frame_is_rtr(frame) ? 1u : 0u;
    slot->frame.header.dlc = frame->dlc;
    slot->frame.buffer = slot->data;
    slot->frame.buffer_len = leaf_can_frame_is_rtr(frame) ? 0u : frame->dlc;

    if (twai_node_transmit(s_node, &slot->frame, 0) != ESP_OK) {
        (void)xQueueSend(s_free_slots, &idx, 0);
        s_stats.tx_dropped++;
        return false;
    }
    return true;
}

void can_twai_service(void)
{
    if (!s_stats.running) {
        return;
    }
    if (s_bus_off) {
        if (!s_recovering && twai_node_recover(s_node) == ESP_OK) {
            s_recovering = true;
            ESP_LOGW(TAG, "bus-off, recovering");
        }
        return;
    }
    if (s_recovering) {
        s_recovering = false;
        s_stats.restarts++;
        ESP_LOGI(TAG, "recovered from bus-off");
    }
    twai_node_status_t status;
    if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
        s_stats.tec = status.tx_error_count;
        s_stats.rec = status.rx_error_count;
        s_stats.state = map_state(status.state);
    }
    /* Safety net: once the driver reports every queued frame done, all slots must be
     * free again. If some were lost (e.g. around a bus-off) put them back. */
    if (uxQueueMessagesWaiting(s_free_slots) < TX_SLOTS && twai_node_transmit_wait_all_done(s_node, 0) == ESP_OK) {
        if (uxQueueMessagesWaiting(s_free_slots) < TX_SLOTS) {
            refill_slots();
        }
    }
}

void can_twai_get_stats(bridge_bus_stats_t *out)
{
    *out = s_stats;
}
