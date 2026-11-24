/* Minimal host stub of ACAN2515.h - only the CANMessage type is needed. */
#pragma once
#include <stdint.h>

class CANMessage {
public:
    uint32_t id = 0;
    bool ext = false;
    bool rtr = false;
    uint8_t idx = 0;
    uint8_t len = 0;
    uint8_t data[8] = {0};
};
