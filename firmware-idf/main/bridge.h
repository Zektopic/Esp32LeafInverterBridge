/*
 * bridge.h - the CAN bridge task and its thread-safe interface.
 *
 * Both CAN drivers post received frames into one queue; a single high priority
 * task runs leaf_inverter_process() on them in arrival order and transmits the
 * results. Nothing else touches the leaf_inverter_t, so no locking is needed on
 * the hot path. The web server only sees snapshots.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "bridge_settings.h"
#include "leaf_can.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx;
    uint32_t tx;
    uint32_t tx_dropped;
    uint32_t rx_overflow;   /* frames lost in the driver / controller */
    uint32_t bus_off;       /* bus-off events */
    bool running;
} bridge_bus_stats_t;

typedef struct {
    uint32_t uptime_s;
    leaf_shifter_t shifter;
    int16_t torque_request_vcm;   /* 0.25 Nm */
    int16_t torque_request_out;   /* 0.25 Nm */
    int16_t torque_response;      /* 0.5 Nm */
    uint8_t charge_status;
    bool ac_charging;
    bool soc_valid;
    uint16_t soc_permille;
    leaf_inverter_stats_t inverter;
    bridge_bus_stats_t bus[LEAF_BUS_COUNT];
    uint32_t queue_overflow;      /* frames dropped because the bridge queue was full */
    uint32_t queue_high_water;
} bridge_status_t;

typedef enum {
    BRIDGE_SETTINGS_SAVED,
    BRIDGE_SETTINGS_UNCHANGED,
    BRIDGE_SETTINGS_NOT_PARKED,
    BRIDGE_SETTINGS_STORAGE_ERROR,
} bridge_settings_result_t;

/* Start the drivers and the bridge task with the given settings. */
esp_err_t bridge_start(const bridge_settings_t *settings);

/* Called by the CAN drivers. The ISR variant may set *woken. */
void bridge_post_from_isr(leaf_bus_t bus, const leaf_can_frame_t *frame, BaseType_t *woken);
void bridge_post(leaf_bus_t bus, const leaf_can_frame_t *frame);

void bridge_get_status(bridge_status_t *out);
void bridge_get_settings(bridge_settings_t *out);
bool bridge_is_parked(void);
bool bridge_buses_running(void);

/* Validate, persist and apply new settings. Safe from any task. */
bridge_settings_result_t bridge_request_settings(const bridge_settings_t *settings);

#ifdef __cplusplus
}
#endif
