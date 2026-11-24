/*
 * leaf_can_signals.h - encode / decode helpers for individual EV-CAN signals.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * All signals are big-endian (Motorola) as on the car. Getters never modify the
 * frame; setters only touch the bits that belong to the signal and do NOT update
 * the checksum - call leaf_can_frame_set_crc8() once all edits are done.
 * Callers are expected to check the frame id / dlc before using a helper.
 */
#ifndef LEAF_CAN_SIGNALS_H
#define LEAF_CAN_SIGNALS_H

#include <stdbool.h>
#include <stdint.h>

#include "leaf_can_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 0x11A shifter status --------------------------------------------------- */
typedef enum {
    LEAF_SHIFTER_PARK    = 0,
    LEAF_SHIFTER_REVERSE = 2,
    LEAF_SHIFTER_NEUTRAL = 3,
    LEAF_SHIFTER_DRIVE   = 4,
} leaf_shifter_t;

/* Gear from data[0] high nibble (0x00 P, 0x20 R, 0x30 N, 0x40 D). Unknown values read as PARK. */
leaf_shifter_t leaf_can_11a_get_shifter(const leaf_can_frame_t *frame);
/* ECO switch flag (data[1] bit 4), as used by the original inverter-upgrade code. */
bool leaf_can_11a_get_eco(const leaf_can_frame_t *frame);
const char *leaf_shifter_name(leaf_shifter_t shifter);

/* ---- 0x1D4 torque request (VCM -> inverter) ---------------------------------- */
#define LEAF_TORQUE_REQUEST_MIN  (-2048) /* 12-bit signed, 0.25 Nm per bit */
#define LEAF_TORQUE_REQUEST_MAX  (2047)

/* Signed 12-bit request in data[2] (bits 11..4) and data[3] high nibble (bits 3..0). */
int16_t leaf_can_1d4_get_torque_request(const leaf_can_frame_t *frame);
/* Saturates to [-2048, 2047]; keeps data[3] low nibble. Returns true if the value was clamped. */
bool leaf_can_1d4_set_torque_request(leaf_can_frame_t *frame, int32_t quarter_nm);

/* ---- 0x1DA torque response (inverter -> VCM) --------------------------------- */
#define LEAF_TORQUE_RESPONSE_MIN (-1024) /* 11-bit signed, 0.5 Nm per bit */
#define LEAF_TORQUE_RESPONSE_MAX (1023)

/* Signed 11-bit response in data[2] bits 2..0 and data[3]. */
int16_t leaf_can_1da_get_torque_response(const leaf_can_frame_t *frame);
/* Saturates to [-1024, 1023]; keeps data[2] bits 7..3. Returns true if the value was clamped. */
bool leaf_can_1da_set_torque_response(leaf_can_frame_t *frame, int32_t half_nm);

/* ---- 0x1DB LBC status -------------------------------------------------------- */
/* HV battery current, 11-bit signed, 0.5 A per bit (positive = discharge). */
int16_t leaf_can_1db_get_current_half_amps(const leaf_can_frame_t *frame);
/* HV battery voltage, 10-bit unsigned, 0.5 V per bit. */
uint16_t leaf_can_1db_get_voltage_half_volts(const leaf_can_frame_t *frame);

/* ---- 0x1F2 VCM charge status ------------------------------------------------- */
#define LEAF_CHARGE_STATUS_SLOW         0x20u /* AC (slow) charging */
#define LEAF_CHARGE_STATUS_QUICK_START  0x40u
#define LEAF_CHARGE_STATUS_IDLE         0x60u
#define LEAF_CHARGE_STATUS_QUICK        0xC0u
#define LEAF_CHARGE_STATUS_QUICK_END    0xE0u

static inline uint8_t leaf_can_1f2_get_charge_status(const leaf_can_frame_t *frame)
{
    return frame->data[2];
}

/* ---- 0x55B LBC state of charge ----------------------------------------------- */
/* 10-bit SOC in 0.1 % (data[0] bits 7..0, data[1] bits 7..6). */
uint16_t leaf_can_55b_get_soc_permille(const leaf_can_frame_t *frame);

/* ---- helpers ------------------------------------------------------------------ */
/* Multiply by a factor given in 1/1000 (1600 = x1.6), rounding toward zero like the original float code. */
static inline int32_t leaf_scale_permille(int32_t value, uint16_t permille)
{
    return (int32_t)(((int64_t)value * (int64_t)permille) / 1000);
}

#ifdef __cplusplus
}
#endif

#endif /* LEAF_CAN_SIGNALS_H */
