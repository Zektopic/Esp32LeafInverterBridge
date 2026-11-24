/*
 * leaf_can_signals.c - encode / decode helpers for individual EV-CAN signals.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "leaf_can_signals.h"

/* Sign-extend the lowest `bits` bits of `raw`. */
static int32_t sign_extend(uint32_t raw, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1u);
    const uint32_t mask = (1u << bits) - 1u;
    raw &= mask;
    return (raw & sign) ? (int32_t)raw - (int32_t)(1u << bits) : (int32_t)raw;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi, bool *clamped)
{
    if (v < lo) {
        *clamped = true;
        return lo;
    }
    if (v > hi) {
        *clamped = true;
        return hi;
    }
    return v;
}

/* ---- 0x11A ------------------------------------------------------------------- */
leaf_shifter_t leaf_can_11a_get_shifter(const leaf_can_frame_t *frame)
{
    switch (frame->data[0] & 0xF0u) {
    case 0x20u:
        return LEAF_SHIFTER_REVERSE;
    case 0x30u:
        return LEAF_SHIFTER_NEUTRAL;
    case 0x40u:
        return LEAF_SHIFTER_DRIVE;
    case 0x00u:
    default:
        return LEAF_SHIFTER_PARK;
    }
}

bool leaf_can_11a_get_eco(const leaf_can_frame_t *frame)
{
    return (frame->data[1] & 0x10u) != 0u;
}

const char *leaf_shifter_name(leaf_shifter_t shifter)
{
    switch (shifter) {
    case LEAF_SHIFTER_REVERSE:
        return "R";
    case LEAF_SHIFTER_NEUTRAL:
        return "N";
    case LEAF_SHIFTER_DRIVE:
        return "D";
    case LEAF_SHIFTER_PARK:
    default:
        return "P";
    }
}

/* ---- 0x1D4 ------------------------------------------------------------------- */
int16_t leaf_can_1d4_get_torque_request(const leaf_can_frame_t *frame)
{
    const uint32_t raw = ((uint32_t)frame->data[2] << 4) | ((uint32_t)frame->data[3] >> 4);
    return (int16_t)sign_extend(raw, 12u);
}

bool leaf_can_1d4_set_torque_request(leaf_can_frame_t *frame, int32_t quarter_nm)
{
    bool clamped = false;
    const int32_t v = clamp_i32(quarter_nm, LEAF_TORQUE_REQUEST_MIN, LEAF_TORQUE_REQUEST_MAX, &clamped);
    const uint32_t raw = (uint32_t)v & 0x0FFFu;
    frame->data[2] = (uint8_t)(raw >> 4);
    frame->data[3] = (uint8_t)(((raw & 0x0Fu) << 4) | (frame->data[3] & 0x0Fu));
    return clamped;
}

/* ---- 0x1DA ------------------------------------------------------------------- */
int16_t leaf_can_1da_get_torque_response(const leaf_can_frame_t *frame)
{
    const uint32_t raw = (((uint32_t)frame->data[2] & 0x07u) << 8) | frame->data[3];
    return (int16_t)sign_extend(raw, 11u);
}

bool leaf_can_1da_set_torque_response(leaf_can_frame_t *frame, int32_t half_nm)
{
    bool clamped = false;
    const int32_t v = clamp_i32(half_nm, LEAF_TORQUE_RESPONSE_MIN, LEAF_TORQUE_RESPONSE_MAX, &clamped);
    const uint32_t raw = (uint32_t)v & 0x07FFu;
    frame->data[2] = (uint8_t)((frame->data[2] & 0xF8u) | (raw >> 8));
    frame->data[3] = (uint8_t)(raw & 0xFFu);
    return clamped;
}

/* ---- 0x1DB ------------------------------------------------------------------- */
int16_t leaf_can_1db_get_current_half_amps(const leaf_can_frame_t *frame)
{
    const uint32_t raw = ((uint32_t)frame->data[0] << 3) | ((uint32_t)frame->data[1] >> 5);
    return (int16_t)sign_extend(raw, 11u);
}

uint16_t leaf_can_1db_get_voltage_half_volts(const leaf_can_frame_t *frame)
{
    return (uint16_t)(((uint32_t)frame->data[2] << 2) | ((uint32_t)frame->data[3] >> 6));
}

/* ---- 0x55B ------------------------------------------------------------------- */
uint16_t leaf_can_55b_get_soc_permille(const leaf_can_frame_t *frame)
{
    return (uint16_t)(((uint32_t)frame->data[0] << 2) | (((uint32_t)frame->data[1] & 0xC0u) >> 6));
}
