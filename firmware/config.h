//——————————————————————————————————————————————————————————————————————————————
// Esp32LeafInverterBridge - Arduino firmware build configuration
// Original authors: Adam Saiyad, Julius Calzada
// SPDX-License-Identifier: GPL-3.0-only
//
// Everything that describes the board or the build lives here. The CAN message
// logic itself is in lib/leaf_can and is shared with the ESP-IDF firmware.
//——————————————————————————————————————————————————————————————————————————————
#ifndef CONFIG_H
#define CONFIG_H

#define FIRMWARE_VERSION "2.0.0"

//——————————————————————————————————————————————————————————————————————————————
// Wi-Fi access point and OTA credentials
// CHANGE THE PASSWORD BEFORE INSTALLING THE BRIDGE IN A CAR. Anyone who can join
// this network can change the torque settings and flash new firmware.
// WPA2 requires at least 8 characters.
//——————————————————————————————————————————————————————————————————————————————
#define WIFI_AP_SSID        "Can-bridgeINV"
#define WIFI_AP_PASSWORD    "Password"
#define WIFI_AP_CHANNEL     6
#define OTA_USERNAME        "admin"
#define OTA_PASSWORD        WIFI_AP_PASSWORD

//——————————————————————————————————————————————————————————————————————————————
// Debug output on the USB serial port (115200 baud). Never blocks if no
// terminal is connected.
//——————————————————————————————————————————————————————————————————————————————
//#define SERIAL_DEBUG

//——————————————————————————————————————————————————————————————————————————————
// CAN buses
//   VCM bus      = ESP32 internal TWAI controller ("CAN2" in v1.x)
//   Inverter bus = MCP2515 on SPI                 ("CAN1" in v1.x)
//——————————————————————————————————————————————————————————————————————————————
#define CAN_BITRATE         500000UL

// ESP32 TWAI (SJA1000 compatible) pins - same defaults as arduino-CAN used.
#define VCM_CAN_TX_PIN      5
#define VCM_CAN_RX_PIN      4

// MCP2515 wiring (reference board)
//   ESP32 GPIO26 -> MCP2515 SCK
//   ESP32 GPIO19 -> MCP2515 SI   (MOSI)
//   ESP32 GPIO18 <- MCP2515 SO   (MISO)
//   ESP32 GPIO15 -> MCP2515 CS
//   ESP32 GPIO22 <- MCP2515 INT
#define MCP2515_SCK_PIN     26
#define MCP2515_MOSI_PIN    19
#define MCP2515_MISO_PIN    18
#define MCP2515_CS_PIN      15
#define MCP2515_INT_PIN     22
#define MCP2515_QUARTZ_HZ   (8UL * 1000UL * 1000UL) // crystal on the MCP2515 module (8 or 16 MHz)

//——————————————————————————————————————————————————————————————————————————————
// Status LED: slow blink = both CAN controllers running, fast blink = a CAN
// controller failed to start.
//——————————————————————————————————————————————————————————————————————————————
#define STATUS_LED_PIN      2

#endif // CONFIG_H
