/*
 * Differential test: original v1.3.0 Arduino handler vs leaf_inverter_process().
 *
 * Usage: diff_original <110|160> <seed> [ticks]
 *
 * Both implementations receive the same randomised traffic (one 10 ms tick of a
 * simulated drive at a time). Every frame each implementation would transmit is
 * recorded as (bus, frame) and the two sequences are compared one by one.
 *
 * Intentional differences, which the comparison accounts for:
 *   1. Saturation: when scaling pushes a torque request outside the 12-bit range,
 *      v1.3.0 wraps (e.g. +560 Nm becomes -464 Nm, i.e. sudden regen) and the
 *      library saturates. The test asserts the library output is the saturated
 *      value and counts how often the original wrapped.
 *   2. Regen scaling: v1.3.0 negates with one's complement (~x), so its result can
 *      differ by 1 LSB (0.25 Nm) from the exact x1.1. Allowed: |delta| <= 1 LSB.
 *   3. 0x1F2 AC-charging guard: v1.3.0 never updated charging_state, so the guard
 *      was dead code. It is disabled in the library config for this test (and
 *      covered by the unit tests instead).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "reference/can_driver.h"
#include "reference/can_bridge_manager_leaf.h"
#include "reference/canframe.h"

extern "C" {
#include "leaf_can.h"
}

/* ---- glue required by the reference sources ------------------------------------ */
uint8_t Vehicle_Selection = 0;
uint8_t Inverter_Upgrade_110Kw_160Kw = 0;

struct Tx {
    int bus; /* 1 = inverter (MCP2515 CAN1), 2 = VCM (TWAI CAN2) */
    leaf_can_frame_t frame;
};

static std::vector<Tx> g_ref_out;
static std::vector<Tx> g_new_out;

static void record_ref(int bus, const can_frame_t &f)
{
    Tx t;
    t.bus = bus;
    t.frame = leaf_can_frame_make(f.can_id, f.can_dlc, f.data);
    g_ref_out.push_back(t);
}

void buffer_send_can0(can_frame_t frame) { record_ref(0, frame); }
void buffer_send_can1(can_frame_t frame) { record_ref(1, frame); }
void buffer_send_can2(can_frame_t frame) { record_ref(2, frame); }
bool CAN1_NewFrameIsAvailable(void) { return false; }
void CAN1_ReadNewFrame(CANMessage &) {}

/* ---- new implementation --------------------------------------------------------- */
static int bus_number(leaf_bus_t b) { return b == LEAF_BUS_INVERTER ? 1 : 2; }

static void emit_new(void *, leaf_bus_t bus, const leaf_can_frame_t *frame)
{
    g_new_out.push_back(Tx{bus_number(bus), *frame});
}

/* ---- traffic generator --------------------------------------------------------------- */
struct Input {
    int bus;
    leaf_can_frame_t frame;
};

static std::mt19937 g_rng;

static uint8_t rnd8() { return (uint8_t)(g_rng() & 0xFF); }
static int rnd_range(int lo, int hi) { return lo + (int)(g_rng() % (uint32_t)(hi - lo + 1)); }

static leaf_can_frame_t random_frame(uint32_t id, uint8_t dlc)
{
    uint8_t d[8];
    for (auto &b : d) b = rnd8();
    return leaf_can_frame_make(id, dlc, d);
}

static leaf_can_frame_t with_crc(leaf_can_frame_t f)
{
    leaf_can_frame_set_crc8(&f);
    return f;
}

static int16_t random_torque()
{
    switch (rnd_range(0, 9)) {
    case 0: return (int16_t)rnd_range(-2048, 2047);  /* full range, exercises overflow */
    case 1: return (int16_t)rnd_range(1200, 2047);   /* hard acceleration */
    case 2: return (int16_t)rnd_range(-2048, -1700); /* extreme regen */
    case 3: return 0;
    default: return (int16_t)rnd_range(-1100, 1100);
    }
}

/* IDs the handler does not treat specially; random traffic. */
static const uint32_t k_other_ids[] = {0x174, 0x176, 0x180, 0x1CA, 0x1DC, 0x245, 0x260, 0x292, 0x2DE,
                                       0x355, 0x380, 0x390, 0x50A, 0x50B, 0x54A, 0x54B, 0x54C, 0x59B,
                                       0x59C, 0x5A9, 0x5B3, 0x5BC, 0x5BF, 0x5C0, 0x625, 0x793};

static std::vector<Input> make_tick(int tick, uint8_t gear_byte)
{
    std::vector<Input> in;

    leaf_can_frame_t f11a = random_frame(LEAF_ID_SHIFTER_STATUS, 8);
    f11a.data[0] = (uint8_t)(gear_byte | (rnd8() & 0x0F));
    in.push_back({2, f11a});

    leaf_can_frame_t f1d4 = random_frame(LEAF_ID_TORQUE_REQUEST, 8);
    (void)leaf_can_1d4_set_torque_request(&f1d4, random_torque());
    in.push_back({2, with_crc(f1d4)});

    leaf_can_frame_t f1f2 = random_frame(LEAF_ID_VCM_CHARGE_STATUS, 8);
    static const uint8_t states[] = {0x60, 0x20, 0x40, 0xC0, 0xE0};
    f1f2.data[2] = rnd_range(0, 5) == 5 ? rnd8() : states[rnd_range(0, 4)];
    in.push_back({2, with_crc(f1f2)});

    in.push_back({2, with_crc(random_frame(LEAF_ID_LBC_STATUS, 8))});

    if (tick % 2 == 0) in.push_back({2, random_frame(LEAF_ID_TIMEBASE_20MS, 8)});
    if (tick % 10 == 3) in.push_back({2, random_frame(LEAF_ID_TIMEBASE_100MS, 8)});
    if (tick % 10 == 7) in.push_back({2, with_crc(random_frame(LEAF_ID_LBC_SOC, 8))});
    if (rnd_range(0, 200) == 0) in.push_back({2, random_frame(LEAF_ID_WAKEUP_603, (uint8_t)rnd_range(0, 8))});

    leaf_can_frame_t f1da = random_frame(LEAF_ID_TORQUE_RESPONSE, 8);
    (void)leaf_can_1da_set_torque_response(&f1da, rnd_range(-1024, 1023));
    in.push_back({1, with_crc(f1da)});

    const int others = rnd_range(0, 4);
    for (int i = 0; i < others; i++) {
        const uint32_t id = k_other_ids[rnd_range(0, (int)(sizeof(k_other_ids) / sizeof(k_other_ids[0])) - 1)];
        in.push_back({rnd_range(1, 2), random_frame(id, (uint8_t)rnd_range(0, 8))});
    }

    /* shuffle the order inside the tick */
    for (size_t i = in.size(); i > 1; i--) {
        std::swap(in[i - 1], in[(size_t)rnd_range(0, (int)i - 1)]);
    }
    return in;
}

/* ---- comparison ----------------------------------------------------------------------- */
struct Counters {
    unsigned long frames = 0, exact = 0, regen_1lsb = 0, original_wrapped = 0;
};

static bool same(const leaf_can_frame_t &a, const leaf_can_frame_t &b)
{
    return a.id == b.id && a.dlc == b.dlc && memcmp(a.data, b.data, a.dlc) == 0;
}

static void fail(const char *why, unsigned long tick, size_t idx, const Tx &r, const Tx &n)
{
    fprintf(stderr, "MISMATCH (%s) at tick %lu, output #%zu\n", why, tick, idx);
    fprintf(stderr, "  original: bus %d id %03X dlc %u data", r.bus, (unsigned)r.frame.id, r.frame.dlc);
    for (int i = 0; i < r.frame.dlc; i++) fprintf(stderr, " %02X", r.frame.data[i]);
    fprintf(stderr, "\n  library : bus %d id %03X dlc %u data", n.bus, (unsigned)n.frame.id, n.frame.dlc);
    for (int i = 0; i < n.frame.dlc; i++) fprintf(stderr, " %02X", n.frame.data[i]);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <110|160> <seed> [ticks]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const int setting = atoi(argv[1]);
    const unsigned seed = (unsigned)strtoul(argv[2], nullptr, 10);
    const unsigned long ticks = argc > 3 ? strtoul(argv[3], nullptr, 10) : 200000UL;
    g_rng.seed(seed);

    leaf_inverter_upgrade_t upgrade;
    if (setting == 110) {
        upgrade = LEAF_INVERTER_UPGRADE_110KW;
        Inverter_Upgrade_110Kw_160Kw = 0;
    } else if (setting == 160) {
        upgrade = LEAF_INVERTER_UPGRADE_160KW;
        Inverter_Upgrade_110Kw_160Kw = 1;
    } else {
        fprintf(stderr, "setting must be 110 or 160\n");
        return EXIT_FAILURE;
    }

    leaf_inverter_config_t cfg;
    leaf_inverter_default_config(&cfg, upgrade);
    cfg.ac_charge_guard = false; /* difference 3 */
    leaf_inverter_t inv;
    leaf_inverter_init(&inv, &cfg);

    static const uint8_t gears[] = {0x00, 0x20, 0x30, 0x40, 0x40, 0x40};
    uint8_t gear = 0x00;
    long next_gear_change = 0;
    Counters c;

    for (unsigned long tick = 0; tick < ticks; tick++) {
        if ((long)tick >= next_gear_change) {
            gear = gears[rnd_range(0, 5)];
            next_gear_change = (long)tick + rnd_range(20, 600);
        }
        for (const Input &in : make_tick((int)tick, gear)) {
            g_ref_out.clear();
            g_new_out.clear();

            /* original */
            can_frame_t rf;
            memset(&rf, 0, sizeof(rf));
            rf.can_id = in.frame.id;
            rf.can_dlc = in.frame.dlc;
            memcpy(rf.data, in.frame.data, sizeof(rf.data));
            LEAF_CAN_Handler((uint8_t)in.bus, rf);

            /* library */
            const leaf_bus_t from = in.bus == 1 ? LEAF_BUS_INVERTER : LEAF_BUS_VCM;
            leaf_can_frame_t nf = in.frame;
            const bool was_drive = inv.shifter == LEAF_SHIFTER_DRIVE;
            if (leaf_inverter_process(&inv, from, &nf, emit_new, nullptr)) {
                g_new_out.push_back(Tx{bus_number(leaf_bus_other(from)), nf});
            }

            if (g_ref_out.size() != g_new_out.size()) {
                fprintf(stderr, "MISMATCH: output count %zu vs %zu at tick %lu (id %03X)\n", g_ref_out.size(),
                        g_new_out.size(), tick, (unsigned)in.frame.id);
                return EXIT_FAILURE;
            }
            for (size_t i = 0; i < g_ref_out.size(); i++) {
                const Tx &r = g_ref_out[i];
                const Tx &n = g_new_out[i];
                c.frames++;
                if (r.bus != n.bus) fail("bus", tick, i, r, n);
                if (same(r.frame, n.frame)) {
                    c.exact++;
                    continue;
                }
                /* Only a translated torque request may differ. */
                if (n.frame.id != LEAF_ID_TORQUE_REQUEST || !was_drive) fail("content", tick, i, r, n);
                const int req = leaf_can_1d4_get_torque_request(&in.frame);
                const uint16_t mult = req < 0 ? cfg.regen_torque_permille : cfg.drive_torque_permille;
                const int32_t exact = leaf_scale_permille(req, mult);
                const int got_new = leaf_can_1d4_get_torque_request(&n.frame);
                const int got_ref = leaf_can_1d4_get_torque_request(&r.frame);
                for (int b : {0, 1, 4, 5, 6}) {
                    if (r.frame.data[b] != n.frame.data[b]) fail("1D4 payload byte", tick, i, r, n);
                }
                if ((r.frame.data[3] & 0x0F) != (n.frame.data[3] & 0x0F)) fail("1D4 low nibble", tick, i, r, n);
                if (!leaf_can_frame_crc8_ok(&n.frame)) fail("1D4 crc", tick, i, r, n);
                if (exact > LEAF_TORQUE_REQUEST_MAX || exact < LEAF_TORQUE_REQUEST_MIN) {
                    const int sat = exact > 0 ? LEAF_TORQUE_REQUEST_MAX : LEAF_TORQUE_REQUEST_MIN;
                    if (got_new != sat) fail("not saturated", tick, i, r, n);
                    c.original_wrapped++;
                } else if (req < 0 && abs(got_new - got_ref) <= 1 && got_new == exact) {
                    c.regen_1lsb++;
                } else {
                    fail("torque value", tick, i, r, n);
                }
            }
        }
    }

    printf("diff_original %d kW seed %u: %lu ticks, %lu frames compared, %lu identical, "
           "%lu regen 1-LSB, %lu overflow cases where v1.3.0 wrapped and the library saturated\n",
           setting, seed, ticks, c.frames, c.exact, c.regen_1lsb, c.original_wrapped);
    return EXIT_SUCCESS;
}
