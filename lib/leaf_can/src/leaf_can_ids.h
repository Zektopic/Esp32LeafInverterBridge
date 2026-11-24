/*
 * leaf_can_ids.h - Nissan LEAF / e-NV200 EV-CAN message identifiers.
 *
 * Part of the leaf_can library (Esp32LeafInverterBridge).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Only messages that this project reads, modifies or generates are listed.
 * Sender, period and meaning come from the Dala inverter-upgrade firmware
 * (github.com/dalathegreat/Nissan-LEAF-Inverter-Upgrade) and the Nissan CAN
 * structures it ships with. Treat anything marked "unknown" as opaque.
 */
#ifndef LEAF_CAN_IDS_H
#define LEAF_CAN_IDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Messages observed on the EV-CAN ------------------------------------ */
#define LEAF_ID_SHIFTER_STATUS      0x11Au /* VCM, 10 ms: selected gear in data[0] high nibble */
#define LEAF_ID_TORQUE_REQUEST      0x1D4u /* VCM -> inverter, 10 ms: torque request (0.25 Nm), CRC8 */
#define LEAF_ID_TORQUE_RESPONSE     0x1DAu /* inverter -> VCM, 10 ms: delivered torque (0.5 Nm), CRC8 */
#define LEAF_ID_LBC_STATUS          0x1DBu /* LBC, 10 ms: HV current / voltage, CRC8 */
#define LEAF_ID_LBC_POWER_LIMITS    0x1DCu /* LBC, 10 ms: discharge / charge power limits, CRC8 */
#define LEAF_ID_VCM_CHARGE_STATUS   0x1F2u /* VCM, 10 ms: charge status in data[2], CRC8 */
#define LEAF_ID_TIMEBASE_20MS       0x284u /* VCM side, 20 ms: used as the 40 ms emulation timebase */
#define LEAF_ID_VCM_50B             0x50Bu /* VCM, 100 ms: wake-up / sleep command */
#define LEAF_ID_TIMEBASE_100MS      0x50Cu /* VCM side, 100 ms: used as the 100/500/1000 ms emulation timebase */
#define LEAF_ID_LBC_SOC             0x55Bu /* LBC, 100 ms: state of charge (0.1 %) */
#define LEAF_ID_5A9                 0x5A9u /* unknown, observed only */
#define LEAF_ID_5BC                 0x5BCu /* LBC, 100 ms: capacity bars / SOH */
#define LEAF_ID_5C0                 0x5C0u /* LBC, 500 ms: history data */
#define LEAF_ID_WAKEUP_603          0x603u /* VCM wake-up; triggers the ZE1 wake-up messages */

/* ---- Messages a 2018+ (ZE1) inverter expects but an older car never sends - */
/* The bridge synthesises these towards the inverter (see leaf_inverter_upgrade.h). */
#define LEAF_ID_ZE1_108             0x108u /* 10 ms: rolling counter + CRC table value */
#define LEAF_ID_ZE1_1C2             0x1C2u /* 10 ms: rolling counter 0x50..0x5F (40 kWh specific) */
#define LEAF_ID_ZE1_1CB             0x1CBu /* 10 ms: 4-step rolling sequence */
#define LEAF_ID_ZE1_1ED             0x1EDu /* 10 ms: 4-step rolling sequence (not needed with a 62 kWh pack) */
#define LEAF_ID_ZE1_355             0x355u /* 40 ms: static snapshot, content unknown */
#define LEAF_ID_ZE1_3B8             0x3B8u /* 100 ms: removes U1000 / P318E together with 0x5EB */
#define LEAF_ID_ZE1_4B9             0x4B9u /* 100 ms: rolling counter 0x40..0x4F, removes P3197 / P318E */
#define LEAF_ID_ZE1_5C5             0x5C5u /* 100 ms: removes U214E / U1000 */
#define LEAF_ID_ZE1_5CD             0x5CDu /* 1000 ms: counter in data[1] */
#define LEAF_ID_ZE1_5EB             0x5EBu /* 500 ms: static */
#define LEAF_ID_ZE1_5EC             0x5ECu /* 500 ms: static */
#define LEAF_ID_ZE1_605             0x605u /* on 0x603: ZE1 wake-up */
#define LEAF_ID_ZE1_607             0x607u /* on 0x603: ZE1 wake-up */
#define LEAF_ID_ZE1_625             0x625u /* 100 ms: static, removes U215B */

#ifdef __cplusplus
}
#endif

#endif /* LEAF_CAN_IDS_H */
