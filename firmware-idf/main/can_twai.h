/*
 * can_twai.h - VCM bus on the ESP32 on-chip TWAI controller (ESP-IDF 6 node API).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "bridge.h"
#include "leaf_can.h"

esp_err_t can_twai_start(void);
/* Non-blocking; false if no TX slot is free (frame dropped and counted). */
bool can_twai_send(const leaf_can_frame_t *frame);
/* Bus-off recovery and TX slot housekeeping; call periodically from the bridge task. */
void can_twai_service(void);
void can_twai_get_stats(bridge_bus_stats_t *out);
