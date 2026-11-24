/*
 * network.h - Wi-Fi access point for the configuration page (http://192.168.4.1/).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The SSID and password come from security.c (NVS, falling back to menuconfig).
 * The access point offers WPA2/WPA3 (mixed) with protected management frames.
 *
 * With CONFIG_BRIDGE_QEMU_OPENETH (testing only) the emulated OpenCores Ethernet
 * of Espressif QEMU is used instead, with an address from QEMU's DHCP server.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t network_start(void);

/* IPv4 address the configuration page is reachable on, as bytes a.b.c.d. */
bool network_get_ip(uint8_t ip[4]);

/* Number of connected Wi-Fi clients (0 in the QEMU build). */
int network_client_count(void);

/* Wi-Fi channel of the access point (0 in the QEMU build). */
int network_channel(void);

/* Re-apply the SSID / password from security.c after `delay_ms` (connected
 * clients are disconnected and must rejoin with the new credentials). */
void network_apply_credentials_later(uint32_t delay_ms);
