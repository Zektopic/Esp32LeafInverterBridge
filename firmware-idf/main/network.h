/*
 * network.h - Wi-Fi access point for the configuration page (http://192.168.4.1/).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * With CONFIG_BRIDGE_QEMU_OPENETH (testing only) the emulated OpenCores Ethernet
 * of Espressif QEMU is used instead, with an address from QEMU's DHCP server.
 */
#pragma once

#include "esp_err.h"

esp_err_t network_start(void);
