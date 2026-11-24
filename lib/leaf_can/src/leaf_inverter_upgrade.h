/*
 * leaf_inverter_upgrade.h - CAN translation for fitting a 110 kW / 160 kW (EM57,
 * 2018+ "ZE1") inverter to an older Nissan LEAF / e-NV200.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Based on the Dala inverter-upgrade firmware
 * (github.com/dalathegreat/Nissan-LEAF-Inverter-Upgrade, GPL-3.0).
 *
 * The bridge sits between the VCM side of the EV-CAN and the inverter. For
 * every frame received on either side call leaf_inverter_process():
 *
 *   - the frame is modified in place where needed (torque request / response),
 *   - frames the new inverter expects but the old car never sends are handed to
 *     the emit callback (always destined for the inverter bus),
 *   - the return value says whether the (possibly modified) frame should be
 *     forwarded to the other bus. Forward it AFTER the call returns so that the
 *     transmit order matches the original firmware.
 *
 * The module has no hardware, OS or heap dependency, is not thread safe (call
 * it from a single task) and allocates nothing.
 */
#ifndef LEAF_INVERTER_UPGRADE_H
#define LEAF_INVERTER_UPGRADE_H

#include <stdbool.h>
#include <stdint.h>

#include "leaf_can_frame.h"
#include "leaf_can_signals.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default multipliers, in 1/1000. Override with -D... at build time. */
#ifndef LEAF_TORQUE_MULTIPLIER_110KW_PERMILLE
/* Value carried over from firmware v1.3.0 of this project. Note it REDUCES torque;
 * the Dala upstream firmware uses 1320 (x1.32). */
#define LEAF_TORQUE_MULTIPLIER_110KW_PERMILLE 900u
#endif
#ifndef LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE
/* Upstream uses 1400 and suggests 1400..1600 for a more aggressive pedal feel. */
#define LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE 1600u
#endif
#ifndef LEAF_REGEN_MULTIPLIER_PERMILLE
#define LEAF_REGEN_MULTIPLIER_PERMILLE 1100u
#endif
#ifndef LEAF_EMULATE_1F2_GROUP_DEFAULT
/* 0x1F2-driven ZE1 messages; off in firmware v1.3.0 ("disable for 62 kWh upgrades"). */
#define LEAF_EMULATE_1F2_GROUP_DEFAULT 0
#endif
#ifndef LEAF_MIN_SOC_PERMILLE_FOR_BOOST_DEFAULT
/* Upstream SMALLBATTERY_IN_USE used 700 (70 %) to protect the fuse of 24 kWh packs. 0 = off. */
#define LEAF_MIN_SOC_PERMILLE_FOR_BOOST_DEFAULT 0u
#endif

typedef enum {
    LEAF_BUS_INVERTER = 0, /* inverter side (MCP2515 "CAN1" on the reference board) */
    LEAF_BUS_VCM      = 1, /* VCM / rest-of-car side (ESP32 TWAI "CAN2") */
    LEAF_BUS_COUNT    = 2,
} leaf_bus_t;

static inline leaf_bus_t leaf_bus_other(leaf_bus_t bus)
{
    return (bus == LEAF_BUS_INVERTER) ? LEAF_BUS_VCM : LEAF_BUS_INVERTER;
}

typedef enum {
    LEAF_INVERTER_UPGRADE_110KW    = 0, /* EM57 motor with 110 kW inverter */
    LEAF_INVERTER_UPGRADE_160KW    = 1, /* EM57 motor with 160 kW inverter */
    LEAF_INVERTER_UPGRADE_DISABLED = 2, /* no torque translation */
} leaf_inverter_upgrade_t;

typedef struct {
    bool     modify_torque;            /* translate 0x1D4 / 0x1DA at all */
    uint16_t drive_torque_permille;    /* applied to positive torque requests in D */
    bool     regen_tuning;             /* also scale regen requests */
    uint16_t regen_torque_permille;    /* applied to negative torque requests in D */
    bool     emulate_ze1_messages;     /* synthesise the 0x284 / 0x50C / 0x603 driven messages */
    bool     emulate_1f2_group;        /* synthesise 0x1C2/0x108/0x1CB/0x1ED; leave off with a 62 kWh pack */
    bool     ac_charge_guard;          /* pause emulation while 0x1F2 reports AC charging */
    uint16_t min_soc_permille_for_boost; /* 0 = off; below this SOC (0.1 %) torque is not modified */
} leaf_inverter_config_t;

typedef struct {
    uint32_t frames_in[LEAF_BUS_COUNT];
    uint32_t frames_modified;
    uint32_t frames_generated;
    uint32_t torque_clamped;           /* scaled request did not fit and was saturated */
} leaf_inverter_stats_t;

/* Internal state of the ZE1 message emulation. Exposed only so the struct can be
 * allocated statically; do not touch the fields. */
typedef struct {
    leaf_can_frame_t f108, f1c2, f1cb, f1ed, f355, f3b8, f4b9, f5c5, f5cd, f5eb, f5ec, f605, f607, f625;
    uint8_t ticker_40ms;
    uint8_t ticker_100ms;
    uint8_t content_4b9;
    uint8_t content_3b8;
    uint8_t flip_3b8;
    uint8_t content_5cd;
    uint8_t flip_5cd;
    uint8_t prun_10ms;
    uint8_t content_1c2;
    uint8_t content_108;
} leaf_ze1_emulation_t;

typedef struct {
    leaf_inverter_config_t cfg;
    leaf_shifter_t shifter;
    int16_t  vcm_torque_request;       /* last unmodified 0x1D4 request, 0.25 Nm */
    int16_t  torque_request_out;       /* last 0x1D4 request sent to the inverter, 0.25 Nm */
    int16_t  torque_response_in;       /* last unmodified 0x1DA response, 0.5 Nm */
    uint8_t  charge_status;            /* last 0x1F2 data[2] */
    uint16_t soc_permille;             /* last 0x55B SOC, 0.1 % */
    bool     soc_valid;
    leaf_ze1_emulation_t emu;
    leaf_inverter_stats_t stats;
} leaf_inverter_t;

/* Receives every synthesised frame. `bus` is the bus to transmit it on. */
typedef void (*leaf_can_emit_fn)(void *ctx, leaf_bus_t bus, const leaf_can_frame_t *frame);

/* Configuration matching a web-UI "Inverter Upgrade" choice. */
void leaf_inverter_default_config(leaf_inverter_config_t *cfg, leaf_inverter_upgrade_t upgrade);

void leaf_inverter_init(leaf_inverter_t *inv, const leaf_inverter_config_t *cfg);

/* Change configuration at runtime; keeps learned state and emulation counters. */
void leaf_inverter_set_config(leaf_inverter_t *inv, const leaf_inverter_config_t *cfg);

/*
 * Process one frame received on `from`. May modify *frame and call `emit`
 * (emit may be NULL to discard synthesised frames). Returns true if *frame must
 * be forwarded to leaf_bus_other(from).
 */
bool leaf_inverter_process(leaf_inverter_t *inv, leaf_bus_t from, leaf_can_frame_t *frame,
                           leaf_can_emit_fn emit, void *ctx);

/* True while 0x1F2 reports AC charging and the guard is enabled. */
bool leaf_inverter_ac_charging(const leaf_inverter_t *inv);

#ifdef __cplusplus
}
#endif

#endif /* LEAF_INVERTER_UPGRADE_H */
