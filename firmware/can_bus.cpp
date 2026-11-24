//——————————————————————————————————————————————————————————————————————————————
// can_bus.cpp - VCM bus on the ESP32 TWAI controller, inverter bus on an MCP2515.
// SPDX-License-Identifier: GPL-3.0-only
//
// v1.x used the sandeepmistry arduino-CAN library for the internal controller.
// It ran the whole CAN handler inside its interrupt and raced the main loop on
// the shared TX buffers, and on ESP32 rev3 silicon it configured half the bit
// rate (hence the old "use 1000E3 on v3 chips" note). The ESP-IDF TWAI driver
// used here queues frames in its ISR, so all bridge logic runs in loop(), and it
// handles every chip revision.
//——————————————————————————————————————————————————————————————————————————————
#include <Arduino.h>
#include <SPI.h>
#include <ACAN2515.h>
#include <driver/twai.h>

#include "can_bus.h"
#include "config.h"

static SPIClass s_spi(HSPI);
static ACAN2515 s_mcp2515(MCP2515_CS_PIN, s_spi, MCP2515_INT_PIN);
static CanBusStats s_stats;
static bool s_twai_recovering = false;

static bool twai_begin() {
  twai_general_config_t g =
      TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)VCM_CAN_TX_PIN, (gpio_num_t)VCM_CAN_RX_PIN, TWAI_MODE_NORMAL);
  g.rx_queue_len = 64;
  g.tx_queue_len = 32;

  // 500 kbit/s exactly as arduino-CAN configured it in v1.x: 80 MHz / 10 = 125 ns
  // quanta, 1 + 13 + 2 = 16 quanta per bit, sample point 87.5 %, SJW 2.
  twai_timing_config_t t = {};
  t.brp = 80000000UL / (CAN_BITRATE * 16UL);
  t.tseg_1 = 13;
  t.tseg_2 = 2;
  t.sjw = 2;
  t.triple_sampling = false;

  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) {
    return false;
  }
  return twai_start() == ESP_OK;
}

static bool mcp2515_begin() {
  s_spi.begin(MCP2515_SCK_PIN, MCP2515_MISO_PIN, MCP2515_MOSI_PIN);
  ACAN2515Settings settings(MCP2515_QUARTZ_HZ, CAN_BITRATE);
  settings.mReceiveBufferSize = 64;
  settings.mTransmitBuffer0Size = 32; // one hardware TX buffer keeps frames in order
  const uint16_t err = s_mcp2515.begin(settings, [] { s_mcp2515.isr(); });
#ifdef SERIAL_DEBUG
  if (err != 0) {
    Serial.printf("[can] MCP2515 init failed, error 0x%04X\n", err);
  } else {
    Serial.printf("[can] MCP2515 running, %lu bit/s, sample point %lu %%\n",
                  (unsigned long)settings.actualBitRate(), (unsigned long)settings.samplePointFromBitStart());
  }
#endif
  return err == 0;
}

bool can_bus_begin() {
  s_stats.running[LEAF_BUS_VCM] = twai_begin();
  s_stats.running[LEAF_BUS_INVERTER] = mcp2515_begin();
#ifdef SERIAL_DEBUG
  Serial.printf("[can] TWAI (VCM bus) %s\n", s_stats.running[LEAF_BUS_VCM] ? "running" : "FAILED");
#endif
  return s_stats.running[LEAF_BUS_VCM] && s_stats.running[LEAF_BUS_INVERTER];
}

bool can_bus_receive(leaf_bus_t bus, leaf_can_frame_t *frame) {
  if (!s_stats.running[bus]) {
    return false;
  }
  memset(frame, 0, sizeof(*frame));
  if (bus == LEAF_BUS_VCM) {
    twai_message_t m;
    if (twai_receive(&m, 0) != ESP_OK) {
      return false;
    }
    frame->id = m.identifier;
    frame->dlc = m.data_length_code > LEAF_CAN_MAX_DLC ? LEAF_CAN_MAX_DLC : m.data_length_code;
    frame->flags = (m.extd ? LEAF_CAN_FLAG_EXTENDED : 0) | (m.rtr ? LEAF_CAN_FLAG_RTR : 0);
    memcpy(frame->data, m.data, frame->dlc);
  } else {
    if (!s_mcp2515.available()) {
      return false;
    }
    CANMessage m;
    s_mcp2515.receive(m);
    frame->id = m.id;
    frame->dlc = m.len > LEAF_CAN_MAX_DLC ? LEAF_CAN_MAX_DLC : m.len;
    frame->flags = (m.ext ? LEAF_CAN_FLAG_EXTENDED : 0) | (m.rtr ? LEAF_CAN_FLAG_RTR : 0);
    memcpy(frame->data, m.data, frame->dlc);
  }
  s_stats.rx[bus]++;
  return true;
}

bool can_bus_send(leaf_bus_t bus, const leaf_can_frame_t *frame) {
  bool ok = false;
  if (s_stats.running[bus]) {
    if (bus == LEAF_BUS_VCM) {
      twai_message_t m = {};
      m.identifier = frame->id;
      m.extd = leaf_can_frame_is_extended(frame) ? 1 : 0;
      m.rtr = leaf_can_frame_is_rtr(frame) ? 1 : 0;
      m.data_length_code = frame->dlc;
      memcpy(m.data, frame->data, frame->dlc);
      ok = twai_transmit(&m, 0) == ESP_OK;
    } else {
      CANMessage m;
      m.id = frame->id;
      m.ext = leaf_can_frame_is_extended(frame);
      m.rtr = leaf_can_frame_is_rtr(frame);
      m.len = frame->dlc;
      memcpy(m.data, frame->data, frame->dlc);
      ok = s_mcp2515.tryToSend(m);
    }
  }
  if (ok) {
    s_stats.tx[bus]++;
  } else {
    s_stats.tx_dropped[bus]++;
  }
  return ok;
}

void can_bus_service() {
  if (!s_stats.running[LEAF_BUS_VCM]) {
    return;
  }
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) {
    return;
  }
  if (st.state == TWAI_STATE_BUS_OFF && !s_twai_recovering) {
    s_twai_recovering = twai_initiate_recovery() == ESP_OK;
  } else if (st.state == TWAI_STATE_STOPPED && s_twai_recovering) {
    // Recovery finished (128 x 11 recessive bits seen): back on the bus.
    if (twai_start() == ESP_OK) {
      s_twai_recovering = false;
      s_stats.bus_off_recoveries++;
    }
  }
}

CanBusStats can_bus_stats() {
  return s_stats;
}
