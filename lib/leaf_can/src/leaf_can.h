/*
 * leaf_can.h - umbrella header for the leaf_can library.
 *
 * Hardware independent Nissan LEAF / e-NV200 EV-CAN message definitions,
 * checksums, signal codecs and the inverter-upgrade translation logic.
 *
 * Part of Esp32LeafInverterBridge.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LEAF_CAN_H
#define LEAF_CAN_H

#define LEAF_CAN_VERSION_MAJOR 2
#define LEAF_CAN_VERSION_MINOR 0
#define LEAF_CAN_VERSION_PATCH 0
#define LEAF_CAN_VERSION_STRING "2.0.0"

#include "leaf_can_frame.h"
#include "leaf_can_ids.h"
#include "leaf_can_checksum.h"
#include "leaf_can_signals.h"
#include "leaf_inverter_upgrade.h"

#endif /* LEAF_CAN_H */
