/*
 * leaf_can_checksum.h - checksums used by Nissan EV-CAN messages.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LEAF_CAN_CHECKSUM_H
#define LEAF_CAN_CHECKSUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "leaf_can_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-8, polynomial 0x85, init 0x00, no reflection, no final XOR. */
uint8_t leaf_can_crc8(const uint8_t *data, size_t len);

/* Write CRC-8 of data[0..6] into data[7] (0x1D4, 0x1DA, 0x1DB, 0x1DC, 0x1F2, 0x55B...). */
void leaf_can_frame_set_crc8(leaf_can_frame_t *frame);

/* True when data[7] holds the CRC-8 of data[0..6]. */
bool leaf_can_frame_crc8_ok(const leaf_can_frame_t *frame);

/*
 * Nibble checksum: low nibble of data[7] = (sum of all nibbles of data[0..6] + 2) & 0xF.
 * The high nibble of data[7] is preserved.
 */
void leaf_can_frame_set_sum4(leaf_can_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* LEAF_CAN_CHECKSUM_H */
