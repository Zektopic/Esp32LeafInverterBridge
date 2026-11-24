//——————————————————————————————————————————————————————————————————————————————
// can_bus.h - the two CAN controllers of the bridge behind one small interface.
// SPDX-License-Identifier: GPL-3.0-only
//——————————————————————————————————————————————————————————————————————————————
#ifndef CAN_BUS_H
#define CAN_BUS_H

#include <leaf_can.h>

struct CanBusStats {
  uint32_t rx[LEAF_BUS_COUNT];
  uint32_t tx[LEAF_BUS_COUNT];
  uint32_t tx_dropped[LEAF_BUS_COUNT];
  uint32_t bus_off_recoveries;      // VCM (TWAI) bus-off recoveries
  bool     running[LEAF_BUS_COUNT]; // controller initialised successfully
};

// Start both controllers. Returns true if both are running.
bool can_bus_begin();

// Non-blocking receive. Returns false if no frame is waiting.
bool can_bus_receive(leaf_bus_t bus, leaf_can_frame_t *frame);

// Non-blocking transmit into the driver queue. Returns false (and counts a drop) if the queue is full.
bool can_bus_send(leaf_bus_t bus, const leaf_can_frame_t *frame);

// Housekeeping (TWAI bus-off recovery). Call every ~100 ms.
void can_bus_service();

CanBusStats can_bus_stats();

#endif // CAN_BUS_H
