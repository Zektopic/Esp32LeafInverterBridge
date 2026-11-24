/*
 * mcp2515.h - inverter bus on an MCP2515 SPI CAN controller.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "bridge.h"
#include "leaf_can.h"

/* Reset and configure the MCP2515 for 500 kbit/s and start its service task. */
esp_err_t mcp2515_start(void);
/* Non-blocking; false if the TX queue is full (frame dropped and counted). */
bool mcp2515_send(const leaf_can_frame_t *frame);
void mcp2515_get_stats(bridge_bus_stats_t *out);
