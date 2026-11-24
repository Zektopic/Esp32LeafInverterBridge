/*
 * captive_dns.h - DNS server of the captive portal (UDP port 53).
 *
 * Answers every A query with the bridge's address so phones open the
 * configuration page by themselves after joining the access point. The
 * packet logic lives in dns_reply.c (host tested).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t captive_dns_start(void);

/* Number of queries answered since boot. */
uint32_t captive_dns_queries(void);
