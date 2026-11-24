/*
 * Host unit tests for the leaf_can library.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "leaf_can.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                \
        g_checks++;                                                                          \
        if (va_ != vb_) {                                                                    \
            g_failures++;                                                                    \
            fprintf(stderr, "%s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__, __LINE__, #a, \
                    #b, va_, vb_);                                                           \
        }                                                                                    \
    } while (0)

/* ---- capture of synthesised frames ------------------------------------------ */
typedef struct {
    leaf_can_frame_t frames[64];
    leaf_bus_t bus[64];
    size_t count;
} capture_t;

static void capture_emit(void *ctx, leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    capture_t *c = (capture_t *)ctx;
    if (c->count < 64) {
        c->frames[c->count] = *frame;
        c->bus[c->count] = bus;
        c->count++;
    }
}

static size_t count_id(const capture_t *c, uint32_t id)
{
    size_t n = 0;
    for (size_t i = 0; i < c->count; i++) {
        n += (c->frames[i].id == id);
    }
    return n;
}

static leaf_can_frame_t frame8(uint32_t id, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    const uint8_t d[8] = {b0, b1, b2, b3, 0x11, 0x22, 0x33, 0};
    leaf_can_frame_t f = leaf_can_frame_make(id, 8, d);
    leaf_can_frame_set_crc8(&f);
    return f;
}

static leaf_can_frame_t torque_request(int16_t quarter_nm)
{
    leaf_can_frame_t f = frame8(LEAF_ID_TORQUE_REQUEST, 0x6E, 0x6E, 0, 0x05);
    (void)leaf_can_1d4_set_torque_request(&f, quarter_nm);
    leaf_can_frame_set_crc8(&f);
    return f;
}

static void put_in_gear(leaf_inverter_t *inv, uint8_t gear_nibble)
{
    leaf_can_frame_t f = frame8(LEAF_ID_SHIFTER_STATUS, gear_nibble, 0, 0, 0);
    (void)leaf_inverter_process(inv, LEAF_BUS_VCM, &f, NULL, NULL);
}

/* ---- checksum ------------------------------------------------------------------ */
static uint8_t crc8_bitwise(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x85u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void test_crc8(void)
{
    /* Decimal table from the original helper_functions.cpp / Dala firmware. */
    static const uint8_t original[256] = {
        0,133,143,10,155,30,20,145,179,54,60,185,40,173,167,34,227,102,108,233,120,253,247,114,80,213,223,90,203,78,68,193,
        67,198,204,73,216,93,87,210,240,117,127,250,107,238,228,97,160,37,47,170,59,190,180,49,19,150,156,25,136,13,7,130,
        134,3,9,140,29,152,146,23,53,176,186,63,174,43,33,164,101,224,234,111,254,123,113,244,214,83,89,220,77,200,194,71,
        197,64,74,207,94,219,209,84,118,243,249,124,237,104,98,231,38,163,169,44,189,56,50,183,149,16,26,159,14,139,129,4,
        137,12,6,131,18,151,157,24,58,191,181,48,161,36,46,171,106,239,229,96,241,116,126,251,217,92,86,211,66,199,205,72,
        202,79,69,192,81,212,222,91,121,252,246,115,226,103,109,232,41,172,166,35,178,55,61,184,154,31,21,144,1,132,142,11,
        15,138,128,5,148,17,27,158,188,57,51,182,39,162,168,45,236,105,99,230,119,242,248,125,95,218,208,85,196,65,75,206,
        76,201,195,70,215,82,88,221,255,122,112,245,100,225,235,110,175,42,32,165,52,177,187,62,28,153,147,22,135,2,8,141};
    for (int i = 0; i < 256; i++) {
        const uint8_t b = (uint8_t)i;
        CHECK_EQ(leaf_can_crc8(&b, 1), original[i]);
        CHECK_EQ(leaf_can_crc8(&b, 1), crc8_bitwise(&b, 1));
    }
    const uint8_t msg[7] = {0x6E, 0x6E, 0x00, 0x00, 0x01, 0x30, 0x40};
    CHECK_EQ(leaf_can_crc8(msg, 7), crc8_bitwise(msg, 7));

    leaf_can_frame_t f = frame8(0x1DB, 1, 2, 3, 4);
    CHECK(leaf_can_frame_crc8_ok(&f));
    f.data[0] ^= 1;
    CHECK(!leaf_can_frame_crc8_ok(&f));

    /* sum4: nibble sum + 2, high nibble of data[7] preserved. */
    const uint8_t d[8] = {0x12, 0x34, 0, 0, 0, 0, 0, 0xA0};
    leaf_can_frame_t s = leaf_can_frame_make(0x100, 8, d);
    leaf_can_frame_set_sum4(&s);
    CHECK_EQ(s.data[7], 0xA0 | ((1 + 2 + 3 + 4 + 2) & 0xF));
}

/* ---- signals ------------------------------------------------------------------ */
static void test_signals(void)
{
    leaf_can_frame_t f = frame8(LEAF_ID_TORQUE_REQUEST, 0, 0, 0, 0x0A);
    for (int v = LEAF_TORQUE_REQUEST_MIN; v <= LEAF_TORQUE_REQUEST_MAX; v++) {
        CHECK(!leaf_can_1d4_set_torque_request(&f, v));
        CHECK_EQ(leaf_can_1d4_get_torque_request(&f), v);
        CHECK_EQ(f.data[3] & 0x0F, 0x0A); /* low nibble preserved */
    }
    CHECK(leaf_can_1d4_set_torque_request(&f, 5000));
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), LEAF_TORQUE_REQUEST_MAX);
    CHECK(leaf_can_1d4_set_torque_request(&f, -5000));
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), LEAF_TORQUE_REQUEST_MIN);

    /* raw layout: 0x7FF0 -> +2047, 0x8000 -> -2048, 0xFFF0 -> -1 */
    f.data[2] = 0x7F; f.data[3] = 0xF0;
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 2047);
    f.data[2] = 0x80; f.data[3] = 0x00;
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), -2048);
    f.data[2] = 0xFF; f.data[3] = 0xF0;
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), -1);

    leaf_can_frame_t r = frame8(LEAF_ID_TORQUE_RESPONSE, 0, 0, 0xA8, 0);
    for (int v = LEAF_TORQUE_RESPONSE_MIN; v <= LEAF_TORQUE_RESPONSE_MAX; v++) {
        CHECK(!leaf_can_1da_set_torque_response(&r, v));
        CHECK_EQ(leaf_can_1da_get_torque_response(&r), v);
        CHECK_EQ(r.data[2] & 0xF8, 0xA8); /* other bits preserved */
    }
    CHECK(leaf_can_1da_set_torque_response(&r, 2000));
    CHECK_EQ(leaf_can_1da_get_torque_response(&r), LEAF_TORQUE_RESPONSE_MAX);

    leaf_can_frame_t s = frame8(LEAF_ID_SHIFTER_STATUS, 0x40, 0x10, 0, 0);
    CHECK_EQ(leaf_can_11a_get_shifter(&s), LEAF_SHIFTER_DRIVE);
    CHECK(leaf_can_11a_get_eco(&s));
    s.data[0] = 0x2F;
    CHECK_EQ(leaf_can_11a_get_shifter(&s), LEAF_SHIFTER_REVERSE);
    s.data[0] = 0x31;
    CHECK_EQ(leaf_can_11a_get_shifter(&s), LEAF_SHIFTER_NEUTRAL);
    s.data[0] = 0x90; /* unknown -> park */
    CHECK_EQ(leaf_can_11a_get_shifter(&s), LEAF_SHIFTER_PARK);

    /* 0x55B: 85.3 % = 853 = 0b1101010101 -> data0 = 0xD5, data1 bits 7..6 = 01 */
    leaf_can_frame_t soc = frame8(LEAF_ID_LBC_SOC, 0xD5, 0x40, 0, 0);
    CHECK_EQ(leaf_can_55b_get_soc_permille(&soc), 853);

    /* 0x1DB: -10 A = -20 half amps (11 bit), 360 V = 720 half volts */
    leaf_can_frame_t db = frame8(LEAF_ID_LBC_STATUS, 0, 0, 0, 0);
    const uint16_t cur = (uint16_t)(-20 & 0x7FF), volt = 720;
    db.data[0] = (uint8_t)(cur >> 3);
    db.data[1] = (uint8_t)((cur & 7) << 5);
    db.data[2] = (uint8_t)(volt >> 2);
    db.data[3] = (uint8_t)((volt & 3) << 6);
    CHECK_EQ(leaf_can_1db_get_current_half_amps(&db), -20);
    CHECK_EQ(leaf_can_1db_get_voltage_half_volts(&db), 720);

    CHECK_EQ(leaf_scale_permille(1000, 1600), 1600);
    CHECK_EQ(leaf_scale_permille(-1001, 1100), -1101); /* toward zero */
    CHECK_EQ(leaf_scale_permille(7, 900), 6);
}

/* ---- inverter translation ------------------------------------------------------ */
static void test_torque_translation(void)
{
    leaf_inverter_config_t cfg;
    leaf_inverter_t inv;
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
    leaf_inverter_init(&inv, &cfg);

    /* Park: untouched. */
    leaf_can_frame_t f = torque_request(400);
    leaf_can_frame_t orig = f;
    CHECK(leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL));
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);
    CHECK_EQ(inv.vcm_torque_request, 400);

    /* Drive: x1.6, low nibble of data[3] cleared, CRC valid. */
    put_in_gear(&inv, 0x40);
    f = torque_request(400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 640);
    CHECK_EQ(f.data[3] & 0x0F, 0);
    CHECK(leaf_can_frame_crc8_ok(&f));
    CHECK_EQ(f.data[0], 0x6E); /* untouched bytes */

    /* 0x1D4 arriving from the inverter side is not a VCM request: untouched. */
    f = torque_request(400);
    orig = f;
    (void)leaf_inverter_process(&inv, LEAF_BUS_INVERTER, &f, NULL, NULL);
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);

    /* Overflow: 1400 * 1.6 = 2240 > 2047 -> saturate instead of wrapping into regen. */
    f = torque_request(1400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 2047);
    CHECK_EQ(inv.stats.torque_clamped, 1);

    /* Regen: x1.1 */
    f = torque_request(-500);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), -550);

    /* Response: replaced by VCM demand / 2 (0.25 Nm -> 0.5 Nm). */
    leaf_can_frame_t r = frame8(LEAF_ID_TORQUE_RESPONSE, 1, 2, 0x04 | 0xF8, 0x10);
    (void)leaf_inverter_process(&inv, LEAF_BUS_INVERTER, &r, NULL, NULL);
    CHECK_EQ(leaf_can_1da_get_torque_response(&r), -250);
    CHECK_EQ(r.data[2] & 0xF8, 0xF8);
    CHECK(leaf_can_frame_crc8_ok(&r));

    /* floor semantics of the original code for odd negative demands. */
    f = torque_request(-3);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    r = frame8(LEAF_ID_TORQUE_RESPONSE, 1, 2, 0x04, 0x10);
    (void)leaf_inverter_process(&inv, LEAF_BUS_INVERTER, &r, NULL, NULL);
    CHECK_EQ(leaf_can_1da_get_torque_response(&r), -2);

    /* Regen tuning off: regen request and regen response untouched. */
    cfg.regen_tuning = false;
    leaf_inverter_set_config(&inv, &cfg);
    f = torque_request(-500);
    orig = f;
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);
    r = frame8(LEAF_ID_TORQUE_RESPONSE, 1, 2, 0x04, 0x10);
    orig = r;
    (void)leaf_inverter_process(&inv, LEAF_BUS_INVERTER, &r, NULL, NULL);
    CHECK(memcmp(&r, &orig, sizeof(r)) == 0);

    /* Disabled upgrade: torque frames are never touched, even in D. */
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_DISABLED);
    leaf_inverter_set_config(&inv, &cfg);
    f = torque_request(400);
    orig = f;
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);

    /* Short frames are passed through untouched. */
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
    leaf_inverter_set_config(&inv, &cfg);
    f = torque_request(400);
    f.dlc = 4;
    orig = f;
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);

    /* Extended / RTR frames: forwarded and ignored. */
    f = torque_request(400);
    f.flags = LEAF_CAN_FLAG_EXTENDED;
    orig = f;
    CHECK(leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL));
    CHECK(memcmp(&f, &orig, sizeof(f)) == 0);

    /* Leaving D stops modification. */
    put_in_gear(&inv, 0x30);
    f = torque_request(400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 400);
}

static void test_soc_guard(void)
{
    leaf_inverter_config_t cfg;
    leaf_inverter_t inv;
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
    cfg.min_soc_permille_for_boost = 700;
    leaf_inverter_init(&inv, &cfg);
    put_in_gear(&inv, 0x40);

    leaf_can_frame_t f = torque_request(400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 400); /* SOC unknown -> no boost */

    leaf_can_frame_t soc = frame8(LEAF_ID_LBC_SOC, 0xAF, 0x00, 0, 0); /* 700 */
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &soc, NULL, NULL);
    f = torque_request(400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 640);

    soc = frame8(LEAF_ID_LBC_SOC, 0xAE, 0xC0, 0, 0); /* 699 */
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &soc, NULL, NULL);
    f = torque_request(400);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, NULL, NULL);
    CHECK_EQ(leaf_can_1d4_get_torque_request(&f), 400);
}

/* ---- ZE1 message emulation ------------------------------------------------------ */
static void test_emulation_timing(void)
{
    leaf_inverter_config_t cfg;
    leaf_inverter_t inv;
    capture_t cap;
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_110KW);
    leaf_inverter_init(&inv, &cfg);

    /* 0x284 every 20 ms -> 0x355 every second frame. */
    size_t n355 = 0;
    for (int i = 0; i < 10; i++) {
        memset(&cap, 0, sizeof(cap));
        leaf_can_frame_t f = frame8(LEAF_ID_TIMEBASE_20MS, 0, 0, 0, 0);
        (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
        n355 += count_id(&cap, LEAF_ID_ZE1_355);
        for (size_t k = 0; k < cap.count; k++) {
            CHECK_EQ(cap.bus[k], LEAF_BUS_INVERTER);
        }
    }
    CHECK_EQ(n355, 5);

    /* 0x284 from the inverter side generates nothing. */
    memset(&cap, 0, sizeof(cap));
    leaf_can_frame_t f = frame8(LEAF_ID_TIMEBASE_20MS, 0, 0, 0, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_INVERTER, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 0);

    /* 0x50C every 100 ms: 4B9 twice, 625, 5C5, 3B8 each time; 5EC/5EB every 5th; 5CD every 10th. */
    size_t n4b9 = 0, n5ec = 0, n5eb = 0, n5cd = 0, n3b8 = 0;
    uint8_t last_4b9 = 0;
    uint8_t first_5cd_counter = 0xFF, second_5cd_counter = 0xFF;
    for (int i = 0; i < 20; i++) {
        memset(&cap, 0, sizeof(cap));
        f = frame8(LEAF_ID_TIMEBASE_100MS, 0, 0, 0, 0);
        (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
        n4b9 += count_id(&cap, LEAF_ID_ZE1_4B9);
        n5ec += count_id(&cap, LEAF_ID_ZE1_5EC);
        n5eb += count_id(&cap, LEAF_ID_ZE1_5EB);
        n3b8 += count_id(&cap, LEAF_ID_ZE1_3B8);
        for (size_t k = 0; k < cap.count; k++) {
            if (cap.frames[k].id == LEAF_ID_ZE1_4B9) {
                last_4b9 = cap.frames[k].data[0];
                CHECK(last_4b9 >= 0x40 && last_4b9 <= 0x4F);
            }
            if (cap.frames[k].id == LEAF_ID_ZE1_5CD) {
                if (n5cd == 0) first_5cd_counter = cap.frames[k].data[1];
                if (n5cd == 1) second_5cd_counter = cap.frames[k].data[1];
                n5cd++;
            }
        }
        CHECK_EQ(cap.frames[0].id, LEAF_ID_ZE1_4B9);
        CHECK_EQ(cap.frames[1].id, LEAF_ID_ZE1_4B9);
        CHECK_EQ(cap.frames[2].id, LEAF_ID_ZE1_625);
        CHECK_EQ(cap.frames[3].id, LEAF_ID_ZE1_5C5);
        CHECK_EQ(cap.frames[4].id, LEAF_ID_ZE1_3B8);
    }
    CHECK_EQ(n4b9, 40);
    CHECK_EQ(n3b8, 20);
    CHECK_EQ(n5ec, 4);
    CHECK_EQ(n5eb, 4);
    CHECK_EQ(n5cd, 2);
    CHECK_EQ(first_5cd_counter, 0);
    CHECK_EQ(second_5cd_counter, 4);
    CHECK_EQ(last_4b9, 0x40 + (20 % 16)); /* 0x41..0x4F then wraps to 0x40 */

    /* 0x603 -> 0x605 + 0x607. */
    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_WAKEUP_603, 0, 0, 0, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 2);
    CHECK_EQ(cap.frames[0].id, LEAF_ID_ZE1_605);
    CHECK_EQ(cap.frames[1].id, LEAF_ID_ZE1_607);

    /* 0x1F2 group is off by default. */
    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_VCM_CHARGE_STATUS, 0, 0, LEAF_CHARGE_STATUS_IDLE, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 0);
}

static void test_ac_charge_guard(void)
{
    leaf_inverter_config_t cfg;
    leaf_inverter_t inv;
    capture_t cap;
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
    cfg.emulate_1f2_group = true;
    leaf_inverter_init(&inv, &cfg);

    leaf_can_frame_t f = frame8(LEAF_ID_VCM_CHARGE_STATUS, 0, 0, LEAF_CHARGE_STATUS_SLOW, 0);
    memset(&cap, 0, sizeof(cap));
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK(leaf_inverter_ac_charging(&inv));
    CHECK_EQ(cap.count, 0); /* 1F2 group suppressed */

    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_TIMEBASE_100MS, 0, 0, 0, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 1); /* only the first 0x4B9 */
    CHECK_EQ(cap.frames[0].id, LEAF_ID_ZE1_4B9);

    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_TIMEBASE_20MS, 0, 0, 0, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    f = frame8(LEAF_ID_TIMEBASE_20MS, 0, 0, 0, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 0);

    /* Charging finished -> full emulation resumes, including the 1F2 group. */
    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_VCM_CHARGE_STATUS, 0, 0, LEAF_CHARGE_STATUS_IDLE, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK(!leaf_inverter_ac_charging(&inv));
    CHECK_EQ(cap.count, 4);
    CHECK_EQ(cap.frames[0].id, LEAF_ID_ZE1_1C2);
    CHECK_EQ(cap.frames[1].id, LEAF_ID_ZE1_108);
    CHECK_EQ(cap.frames[2].id, LEAF_ID_ZE1_1CB);
    CHECK_EQ(cap.frames[3].id, LEAF_ID_ZE1_1ED);
    /* first transmission uses the start-up content */
    CHECK_EQ(cap.frames[2].data[5], 0x8B);
    CHECK_EQ(cap.frames[2].data[6], 0xE7);

    /* Guard disabled: AC charging no longer suppresses anything. */
    cfg.ac_charge_guard = false;
    leaf_inverter_set_config(&inv, &cfg);
    memset(&cap, 0, sizeof(cap));
    f = frame8(LEAF_ID_VCM_CHARGE_STATUS, 0, 0, LEAF_CHARGE_STATUS_SLOW, 0);
    (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
    CHECK_EQ(cap.count, 4);
}

static void test_1f2_sequences(void)
{
    leaf_inverter_config_t cfg;
    leaf_inverter_t inv;
    capture_t cap;
    leaf_inverter_default_config(&cfg, LEAF_INVERTER_UPGRADE_160KW);
    cfg.emulate_1f2_group = true;
    leaf_inverter_init(&inv, &cfg);

    static const uint8_t lut108[16] = {0x00, 0x85, 0x8F, 0x0A, 0x9B, 0x1e, 0x14, 0x91,
                                       0xb3, 0x36, 0x3c, 0xb9, 0x28, 0xad, 0xa7, 0x22};
    for (int i = 0; i < 40; i++) {
        memset(&cap, 0, sizeof(cap));
        leaf_can_frame_t f = frame8(LEAF_ID_VCM_CHARGE_STATUS, 0, 0, LEAF_CHARGE_STATUS_IDLE, 0);
        (void)leaf_inverter_process(&inv, LEAF_BUS_VCM, &f, capture_emit, &cap);
        CHECK_EQ(cap.count, 4);
        const leaf_can_frame_t *c1c2 = &cap.frames[0], *c108 = &cap.frames[1];
        const leaf_can_frame_t *c1cb = &cap.frames[2], *c1ed = &cap.frames[3];
        /* value sent on iteration i was set at the end of iteration i-1 */
        CHECK_EQ(c1c2->data[0], (i == 0) ? 0x50 : 0x50 + (i % 16));
        CHECK_EQ(c108->data[1], (i == 0) ? 0 : (i % 16));
        CHECK_EQ(c108->data[2], lut108[(i == 0) ? 0 : (i % 16)]);
        /* PRUN10MS tables from v1.3.0 (if/else chain on PRUN10MS 0..3) */
        static const uint8_t cb5[4] = {0x88, 0x89, 0x8A, 0x8B}, cb6[4] = {0xED, 0x68, 0x62, 0xE7};
        static const uint8_t ed1[4] = {0xE0, 0xE1, 0xE2, 0xE3}, ed2[4] = {0x68, 0xED, 0xE7, 0x62};
        CHECK_EQ(c1cb->dlc, 7);
        CHECK_EQ(c1ed->dlc, 3);
        CHECK_EQ(c1cb->data[5], (i == 0) ? 0x8B : cb5[i % 4]);
        CHECK_EQ(c1cb->data[6], (i == 0) ? 0xE7 : cb6[i % 4]);
        CHECK_EQ(c1ed->data[1], (i == 0) ? 0xE0 : ed1[i % 4]);
        CHECK_EQ(c1ed->data[2], (i == 0) ? 0x68 : ed2[i % 4]);
        CHECK_EQ(c1cb->data[0], 0x00);
        CHECK_EQ(c1cb->data[1], 0x09);
        CHECK_EQ(c1ed->data[0], 0xFF);
    }
}

int main(void)
{
    test_crc8();
    test_signals();
    test_torque_translation();
    test_soc_guard();
    test_emulation_timing();
    test_ac_charge_guard();
    test_1f2_sequences();

    printf("leaf_can unit tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
