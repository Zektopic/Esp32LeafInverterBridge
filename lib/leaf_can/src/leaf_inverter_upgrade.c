/*
 * leaf_inverter_upgrade.c - CAN translation for the LEAF inverter upgrade.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Message contents, periods and counter sequences are taken unchanged from the
 * Dala inverter-upgrade firmware and from v1.3.0 of this project.
 */
#include "leaf_inverter_upgrade.h"

#include "leaf_can_checksum.h"
#include "leaf_can_ids.h"

/* Values written to 0x1CB data[5..6] and 0x1ED data[1..2] for each 10 ms step. */
static const uint8_t k_1cb_seq[4][2] = {{0x88, 0xED}, {0x89, 0x68}, {0x8A, 0x62}, {0x8B, 0xE7}};
static const uint8_t k_1ed_seq[4][2] = {{0xE0, 0x68}, {0xE1, 0xED}, {0xE2, 0xE7}, {0xE3, 0x62}};

static leaf_can_frame_t make(uint32_t id, uint8_t dlc, const uint8_t *data)
{
    return leaf_can_frame_make(id, dlc, data);
}

static void emulation_init(leaf_ze1_emulation_t *e)
{
    static const uint8_t d108[] = {0x00, 0x00, 0x00};
    static const uint8_t d1c2[] = {0x50};
    static const uint8_t d1cb[] = {0x00, 0x09, 0xFF, 0xCE, 0x10, 0x8B, 0xE7}; /* start-up sequence */
    static const uint8_t d1ed[] = {0xFF, 0xE0, 0x68};
    static const uint8_t d355[] = {0x14, 0x0A, 0x13, 0x97, 0x10, 0x00, 0x40, 0x00}; /* snapshot from a power run */
    static const uint8_t d3b8[] = {0x7F, 0xE8, 0x01, 0x07, 0xFF};                   /* idle data */
    static const uint8_t d4b9[] = {0x40};
    static const uint8_t d5c5[] = {0x40, 0x01, 0x2F, 0x5E, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t d5cd[] = {0x7A, 0x06, 0xF5, 0x1F, 0xC0};
    static const uint8_t d5eb[] = {0xE0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t d5ec[] = {0x00};
    static const uint8_t d605[] = {0x00};
    static const uint8_t d607[] = {0x00};
    static const uint8_t d625[] = {0x02, 0x00, 0xFF, 0x1D, 0x20, 0x00};

    e->f108 = make(LEAF_ID_ZE1_108, sizeof(d108), d108);
    e->f1c2 = make(LEAF_ID_ZE1_1C2, sizeof(d1c2), d1c2);
    e->f1cb = make(LEAF_ID_ZE1_1CB, sizeof(d1cb), d1cb);
    e->f1ed = make(LEAF_ID_ZE1_1ED, sizeof(d1ed), d1ed);
    e->f355 = make(LEAF_ID_ZE1_355, sizeof(d355), d355);
    e->f3b8 = make(LEAF_ID_ZE1_3B8, sizeof(d3b8), d3b8);
    e->f4b9 = make(LEAF_ID_ZE1_4B9, sizeof(d4b9), d4b9);
    e->f5c5 = make(LEAF_ID_ZE1_5C5, sizeof(d5c5), d5c5);
    e->f5cd = make(LEAF_ID_ZE1_5CD, sizeof(d5cd), d5cd);
    e->f5eb = make(LEAF_ID_ZE1_5EB, sizeof(d5eb), d5eb);
    e->f5ec = make(LEAF_ID_ZE1_5EC, sizeof(d5ec), d5ec);
    e->f605 = make(LEAF_ID_ZE1_605, sizeof(d605), d605);
    e->f607 = make(LEAF_ID_ZE1_607, sizeof(d607), d607);
    e->f625 = make(LEAF_ID_ZE1_625, sizeof(d625), d625);

    e->ticker_40ms = 0;
    e->ticker_100ms = 0;
    e->content_4b9 = 0x40;
    e->content_3b8 = 0;
    e->flip_3b8 = 0;
    e->content_5cd = 0;
    e->flip_5cd = 0;
    e->prun_10ms = 0;
    e->content_1c2 = 0x50;
    e->content_108 = 0;
}

void leaf_inverter_default_config(leaf_inverter_config_t *cfg, leaf_inverter_upgrade_t upgrade)
{
    cfg->modify_torque = (upgrade != LEAF_INVERTER_UPGRADE_DISABLED);
    cfg->drive_torque_permille = (upgrade == LEAF_INVERTER_UPGRADE_160KW)
                                     ? (uint16_t)LEAF_TORQUE_MULTIPLIER_160KW_PERMILLE
                                     : (uint16_t)LEAF_TORQUE_MULTIPLIER_110KW_PERMILLE;
    cfg->regen_tuning = true;
    cfg->regen_torque_permille = (uint16_t)LEAF_REGEN_MULTIPLIER_PERMILLE;
    cfg->emulate_ze1_messages = true;
    cfg->emulate_1f2_group = (LEAF_EMULATE_1F2_GROUP_DEFAULT) != 0;
    cfg->ac_charge_guard = true;
    cfg->min_soc_permille_for_boost = (uint16_t)LEAF_MIN_SOC_PERMILLE_FOR_BOOST_DEFAULT;
}

void leaf_inverter_init(leaf_inverter_t *inv, const leaf_inverter_config_t *cfg)
{
    memset(inv, 0, sizeof(*inv));
    inv->cfg = *cfg;
    inv->shifter = LEAF_SHIFTER_PARK;
    emulation_init(&inv->emu);
}

void leaf_inverter_set_config(leaf_inverter_t *inv, const leaf_inverter_config_t *cfg)
{
    inv->cfg = *cfg;
}

bool leaf_inverter_ac_charging(const leaf_inverter_t *inv)
{
    return inv->cfg.ac_charge_guard && inv->charge_status == LEAF_CHARGE_STATUS_SLOW;
}

static bool torque_mod_allowed(const leaf_inverter_t *inv)
{
    if (!inv->cfg.modify_torque || inv->shifter != LEAF_SHIFTER_DRIVE) {
        return false;
    }
    if (inv->cfg.min_soc_permille_for_boost != 0u &&
        (!inv->soc_valid || inv->soc_permille < inv->cfg.min_soc_permille_for_boost)) {
        return false; /* low SOC: protect the battery fuse (upstream SMALLBATTERY_IN_USE) */
    }
    return true;
}

static void emit_frame(leaf_inverter_t *inv, leaf_can_emit_fn emit, void *ctx, const leaf_can_frame_t *f)
{
    inv->stats.frames_generated++;
    if (emit != NULL) {
        emit(ctx, LEAF_BUS_INVERTER, f);
    }
}

/* floor(v / 2): what the original "unsigned 12-bit * 0.5" arithmetic produced. */
static int32_t floor_half(int32_t v)
{
    return (v >= 0) ? (v / 2) : -((-v + 1) / 2);
}

static void handle_torque_request(leaf_inverter_t *inv, leaf_can_frame_t *frame)
{
    const int16_t request = leaf_can_1d4_get_torque_request(frame);
    int32_t scaled;

    inv->vcm_torque_request = request;
    inv->torque_request_out = request;
    if (!torque_mod_allowed(inv)) {
        return;
    }
    if (request < 0) {
        if (!inv->cfg.regen_tuning) {
            return;
        }
        scaled = leaf_scale_permille(request, inv->cfg.regen_torque_permille);
    } else {
        scaled = leaf_scale_permille(request, inv->cfg.drive_torque_permille);
    }
    if (leaf_can_1d4_set_torque_request(frame, scaled)) {
        inv->stats.torque_clamped++;
    }
    frame->data[3] &= 0xF0u; /* upstream clears the low nibble when re-encoding */
    leaf_can_frame_set_crc8(frame);
    inv->torque_request_out = leaf_can_1d4_get_torque_request(frame);
    inv->stats.frames_modified++;
}

static void handle_torque_response(leaf_inverter_t *inv, leaf_can_frame_t *frame)
{
    inv->torque_response_in = leaf_can_1da_get_torque_response(frame);
    if (!torque_mod_allowed(inv)) {
        return;
    }
    if ((frame->data[2] & 0x04u) != 0u && !inv->cfg.regen_tuning) {
        return; /* regen response and regen tuning disabled */
    }
    /* Report back exactly what the VCM asked for (0.25 Nm -> 0.5 Nm units). */
    (void)leaf_can_1da_set_torque_response(frame, floor_half(inv->vcm_torque_request));
    leaf_can_frame_set_crc8(frame);
    inv->stats.frames_modified++;
}

static void emulate_on_284(leaf_inverter_t *inv, leaf_can_emit_fn emit, void *ctx)
{
    leaf_ze1_emulation_t *e = &inv->emu;
    if (leaf_inverter_ac_charging(inv)) {
        return; /* would interrupt AC charging on 62 kWh LEAFs */
    }
    e->ticker_40ms++;
    if (e->ticker_40ms > 1u) {
        e->ticker_40ms = 0;
        emit_frame(inv, emit, ctx, &e->f355);
    }
}

static void emulate_on_50c(leaf_inverter_t *inv, leaf_can_emit_fn emit, void *ctx)
{
    leaf_ze1_emulation_t *e = &inv->emu;

    e->content_4b9++;
    if (e->content_4b9 > 79u) {
        e->content_4b9 = 64;
    }
    e->f4b9.data[0] = e->content_4b9; /* 0x40..0x4F */
    emit_frame(inv, emit, ctx, &e->f4b9);

    if (leaf_inverter_ac_charging(inv)) {
        return;
    }

    emit_frame(inv, emit, ctx, &e->f4b9);
    emit_frame(inv, emit, ctx, &e->f625);
    emit_frame(inv, emit, ctx, &e->f5c5);
    emit_frame(inv, emit, ctx, &e->f3b8);

    e->content_3b8++;
    if (e->content_3b8 > 14u) {
        e->content_3b8 = 0;
    }
    e->f3b8.data[2] = e->content_3b8; /* 0x00..0x0E */
    if (e->flip_3b8) {
        e->flip_3b8 = 0;
        e->f3b8.data[1] = 0xC8;
    } else {
        e->flip_3b8 = 1;
        e->f3b8.data[1] = 0xE8;
    }

    e->ticker_100ms++;
    if (e->ticker_100ms > 4u) { /* every 500 ms */
        e->ticker_100ms = 0;
        emit_frame(inv, emit, ctx, &e->f5ec);
        emit_frame(inv, emit, ctx, &e->f5eb);
        if (e->flip_5cd == 0u) { /* every 1000 ms */
            e->flip_5cd = 1;
            e->f5cd.data[1] = e->content_5cd;
            emit_frame(inv, emit, ctx, &e->f5cd);
            e->content_5cd = (uint8_t)(e->content_5cd + 4u);
            if (e->content_5cd > 238u) {
                e->content_5cd = 2;
            }
        } else {
            e->flip_5cd = 0;
        }
    }
}

static void emulate_on_1f2(leaf_inverter_t *inv, leaf_can_emit_fn emit, void *ctx)
{
    leaf_ze1_emulation_t *e = &inv->emu;
    if (leaf_inverter_ac_charging(inv)) {
        return;
    }

    emit_frame(inv, emit, ctx, &e->f1c2);
    emit_frame(inv, emit, ctx, &e->f108);
    emit_frame(inv, emit, ctx, &e->f1cb);
    emit_frame(inv, emit, ctx, &e->f1ed);

    e->prun_10ms++;
    if (e->prun_10ms > 3u) {
        e->prun_10ms = 0;
    }
    e->f1cb.data[5] = k_1cb_seq[e->prun_10ms][0];
    e->f1cb.data[6] = k_1cb_seq[e->prun_10ms][1];
    e->f1ed.data[1] = k_1ed_seq[e->prun_10ms][0];
    e->f1ed.data[2] = k_1ed_seq[e->prun_10ms][1];

    e->content_1c2++;
    if (e->content_1c2 > 95u) {
        e->content_1c2 = 80;
    }
    e->f1c2.data[0] = e->content_1c2; /* 0x50..0x5F */

    e->content_108++;
    if (e->content_108 > 0x0Fu) {
        e->content_108 = 0;
    }
    e->f108.data[1] = e->content_108;
    e->f108.data[2] = leaf_can_crc8(&e->content_108, 1u); /* == upstream lookuptable_crc_108[] */
}

bool leaf_inverter_process(leaf_inverter_t *inv, leaf_bus_t from, leaf_can_frame_t *frame,
                           leaf_can_emit_fn emit, void *ctx)
{
    const bool from_vcm = (from == LEAF_BUS_VCM);

    if ((unsigned)from < (unsigned)LEAF_BUS_COUNT) {
        inv->stats.frames_in[from]++;
    }
    if ((frame->flags & (LEAF_CAN_FLAG_EXTENDED | LEAF_CAN_FLAG_RTR)) != 0u) {
        return true; /* the EV-CAN only uses 11-bit data frames: pass anything else through */
    }

    switch (frame->id) {
    case LEAF_ID_SHIFTER_STATUS:
        if (from_vcm && frame->dlc >= 1u) {
            inv->shifter = leaf_can_11a_get_shifter(frame);
        }
        break;

    case LEAF_ID_TORQUE_REQUEST:
        if (from_vcm && frame->dlc == 8u) {
            handle_torque_request(inv, frame);
        }
        break;

    case LEAF_ID_TORQUE_RESPONSE:
        if (!from_vcm && frame->dlc == 8u) {
            handle_torque_response(inv, frame);
        }
        break;

    case LEAF_ID_VCM_CHARGE_STATUS:
        if (from_vcm && frame->dlc >= 3u) {
            inv->charge_status = leaf_can_1f2_get_charge_status(frame);
            if (inv->cfg.emulate_1f2_group) {
                emulate_on_1f2(inv, emit, ctx);
            }
        }
        break;

    case LEAF_ID_LBC_SOC:
        if (from_vcm && frame->dlc >= 2u) {
            inv->soc_permille = leaf_can_55b_get_soc_permille(frame);
            inv->soc_valid = true;
        }
        break;

    case LEAF_ID_TIMEBASE_20MS:
        if (from_vcm && inv->cfg.emulate_ze1_messages) {
            emulate_on_284(inv, emit, ctx);
        }
        break;

    case LEAF_ID_TIMEBASE_100MS:
        if (from_vcm && inv->cfg.emulate_ze1_messages) {
            emulate_on_50c(inv, emit, ctx);
        }
        break;

    case LEAF_ID_WAKEUP_603:
        if (from_vcm && inv->cfg.emulate_ze1_messages) {
            emit_frame(inv, emit, ctx, &inv->emu.f605);
            emit_frame(inv, emit, ctx, &inv->emu.f607);
        }
        break;

    default:
        break;
    }

    return true; /* transparent bridge: everything is forwarded */
}
