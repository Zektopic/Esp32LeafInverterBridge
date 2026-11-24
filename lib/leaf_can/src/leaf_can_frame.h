/*
 * leaf_can_frame.h - hardware independent classic CAN frame type.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Every driver (MCP2515, ESP32 TWAI, SocketCAN, a log replayer...) converts its
 * own frame type to/from leaf_can_frame_t, so the message logic in this library
 * never depends on a particular CAN controller or framework.
 */
#ifndef LEAF_CAN_FRAME_H
#define LEAF_CAN_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEAF_CAN_MAX_DLC        8u

#define LEAF_CAN_FLAG_EXTENDED  0x01u /* 29-bit identifier */
#define LEAF_CAN_FLAG_RTR       0x02u /* remote transmission request */

#define LEAF_CAN_STD_ID_MASK    0x7FFu
#define LEAF_CAN_EXT_ID_MASK    0x1FFFFFFFu

typedef struct {
    uint32_t id;                    /* 11-bit or 29-bit identifier (see flags) */
    uint8_t  dlc;                   /* payload length 0..8 */
    uint8_t  flags;                 /* LEAF_CAN_FLAG_* */
    uint8_t  data[LEAF_CAN_MAX_DLC];
} leaf_can_frame_t;

/* Build a standard (11-bit) data frame. Bytes beyond `dlc` are zeroed. */
static inline leaf_can_frame_t leaf_can_frame_make(uint32_t id, uint8_t dlc, const uint8_t *data)
{
    leaf_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = id & LEAF_CAN_STD_ID_MASK;
    f.dlc = (dlc > LEAF_CAN_MAX_DLC) ? LEAF_CAN_MAX_DLC : dlc;
    if (data != NULL) {
        memcpy(f.data, data, f.dlc);
    }
    return f;
}

static inline bool leaf_can_frame_is_extended(const leaf_can_frame_t *f)
{
    return (f->flags & LEAF_CAN_FLAG_EXTENDED) != 0u;
}

static inline bool leaf_can_frame_is_rtr(const leaf_can_frame_t *f)
{
    return (f->flags & LEAF_CAN_FLAG_RTR) != 0u;
}

/* True for an 11-bit data frame with the given id (what every Leaf message is). */
static inline bool leaf_can_frame_is_std(const leaf_can_frame_t *f, uint32_t id)
{
    return (f->flags & (LEAF_CAN_FLAG_EXTENDED | LEAF_CAN_FLAG_RTR)) == 0u && f->id == id;
}

#ifdef __cplusplus
}
#endif

#endif /* LEAF_CAN_FRAME_H */
