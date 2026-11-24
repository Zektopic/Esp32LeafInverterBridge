//——————————————————————————————————————————————————————————————————————————————
// web_ui.cpp - Wi-Fi access point, configuration page, status API and OTA.
// SPDX-License-Identifier: GPL-3.0-only
//
//   GET  /             configuration page (SPIFFS /index.html)
//   GET  /static/...   page assets (SPIFFS /static/)
//   POST /settings     JSON body from the page; 200 / 400 / 409 / 500 with a message
//   GET  /api/status   live bridge status as JSON
//   WS   /ws           pushes the selected radio button ids (e.g. "Inverter2")
//   GET  /update       ElegantOTA firmware update (HTTP basic auth, P only)
//——————————————————————————————————————————————————————————————————————————————
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <SPIFFS.h>
#include <WiFi.h>

#include "bridge_state.h"
#include "config.h"
#include "web_ui.h"

static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");
static constexpr size_t kMaxSettingsBody = 1024;

// ---- WebSocket -------------------------------------------------------------------
static void send_selected_options(AsyncWebSocketClient *client) {
  const bridge_settings_t s = bridge_settings();
  char id[32];
  for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
    if (bridge_settings_radio_id(&s, static_cast<bridge_setting_id_t>(i), id, sizeof(id))) {
      if (client != nullptr) {
        client->text(id);
      } else {
        s_ws.textAll(id);
      }
    }
  }
}

static void on_ws_event(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *,
                        size_t) {
  if (type == WS_EVT_CONNECT) {
    send_selected_options(client);
  }
}

// ---- POST /settings -------------------------------------------------------------
// index.html v2 posts application/json (raw body); the v1 page posted the JSON as
// a form-encoded "body" parameter. Both are accepted.
static void on_settings_body(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (total > kMaxSettingsBody) {
    return; // rejected in on_settings_request
  }
  if (index == 0) {
    request->_tempObject = calloc(1, total + 1);
  }
  char *buf = static_cast<char *>(request->_tempObject);
  if (buf != nullptr && index + len <= total) {
    memcpy(buf + index, data, len);
  }
}

static void on_settings_request(AsyncWebServerRequest *request) {
  const char *body = static_cast<const char *>(request->_tempObject);
  String param;
  if (body == nullptr && request->hasParam("body", true)) {
    param = request->getParam("body", true)->value();
    body = param.c_str();
  }
  if (body == nullptr || strlen(body) == 0 || strlen(body) > kMaxSettingsBody) {
    request->send(400, "text/plain", "Missing or too large settings body");
    return;
  }

  const bridge_settings_t current = bridge_settings();
  bridge_settings_t next;
  char err[96];
  if (!bridge_settings_parse_json(body, strlen(body), &current, &next, err, sizeof(err))) {
    request->send(400, "text/plain", String("Rejected: ") + err);
    return;
  }

  switch (bridge_request_settings(next)) {
    case SettingsResult::Saved:
      request->send(200, "text/plain", "Settings saved");
      send_selected_options(nullptr);
      break;
    case SettingsResult::Unchanged:
      request->send(200, "text/plain", "Settings unchanged");
      break;
    case SettingsResult::NotParked:
      request->send(409, "text/plain", "Shift to P (park) before changing settings");
      break;
    case SettingsResult::StorageError:
    default:
      request->send(500, "text/plain", "Could not write settings to flash");
      break;
  }
}

// ---- GET /api/status ------------------------------------------------------------
static void on_status(AsyncWebServerRequest *request) {
  const BridgeStatus st = bridge_status();
  const bridge_settings_t s = bridge_settings();
  char soc[8] = "null";
  if (st.soc_valid) {
    snprintf(soc, sizeof(soc), "%.1f", st.soc_permille / 10.0);
  }
  char json[768];
  snprintf(json, sizeof(json),
           "{\"firmware\":\"%s\",\"leaf_can\":\"%s\",\"uptime_s\":%lu,"
           "\"inverter_setting\":\"%s\",\"shifter\":\"%s\","
           "\"torque_request_vcm_nm\":%.2f,\"torque_request_out_nm\":%.2f,\"torque_response_nm\":%.1f,"
           "\"charge_status\":%u,\"ac_charging\":%s,\"soc_percent\":%s,"
           "\"frames_modified\":%lu,\"frames_generated\":%lu,\"torque_clamped\":%lu,"
           "\"vcm_bus\":{\"running\":%s,\"rx\":%lu,\"tx\":%lu,\"tx_dropped\":%lu,\"bus_off_recoveries\":%lu},"
           "\"inverter_bus\":{\"running\":%s,\"rx\":%lu,\"tx\":%lu,\"tx_dropped\":%lu}}",
           FIRMWARE_VERSION, LEAF_CAN_VERSION_STRING, (unsigned long)st.uptime_s,
           bridge_setting_option_label(BRIDGE_SETTING_INVERTER, s.value[BRIDGE_SETTING_INVERTER]),
           leaf_shifter_name(st.shifter), st.torque_request_vcm / 4.0, st.torque_request_out / 4.0,
           st.torque_response / 2.0, st.charge_status, st.ac_charging ? "true" : "false",
           soc, (unsigned long)st.inverter.frames_modified,
           (unsigned long)st.inverter.frames_generated, (unsigned long)st.inverter.torque_clamped,
           st.can.running[LEAF_BUS_VCM] ? "true" : "false", (unsigned long)st.can.rx[LEAF_BUS_VCM],
           (unsigned long)st.can.tx[LEAF_BUS_VCM], (unsigned long)st.can.tx_dropped[LEAF_BUS_VCM],
           (unsigned long)st.can.bus_off_recoveries, st.can.running[LEAF_BUS_INVERTER] ? "true" : "false",
           (unsigned long)st.can.rx[LEAF_BUS_INVERTER], (unsigned long)st.can.tx[LEAF_BUS_INVERTER],
           (unsigned long)st.can.tx_dropped[LEAF_BUS_INVERTER]);
  request->send(200, "application/json", json);
}

// ---- setup -------------------------------------------------------------------------
void web_ui_begin() {
  if (!SPIFFS.begin(true)) {
#ifdef SERIAL_DEBUG
    Serial.println("[web] SPIFFS mount failed - upload the web/ folder (pio run -t uploadfs)");
#endif
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL);
#ifdef SERIAL_DEBUG
  Serial.printf("[web] access point \"%s\" at %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
#endif

  s_ws.onEvent(on_ws_event);
  s_server.addHandler(&s_ws);

  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
  });
  s_server.on("/settings", HTTP_POST, on_settings_request, nullptr, on_settings_body);
  s_server.on("/api/status", HTTP_GET, on_status);
  s_server.serveStatic("/static/", SPIFFS, "/static/").setCacheControl("max-age=86400");

  // Refuse firmware updates unless the car is in P. Registered before ElegantOTA
  // so they see the request first; when parked the filter does not match and
  // ElegantOTA handles it. /ota/upload is guarded too, because the car could leave
  // P between ElegantOTA's /ota/start and /ota/upload requests. (The ESP-IDF
  // firmware additionally aborts if the car leaves P during the upload.)
  auto refuse_ota = [](AsyncWebServerRequest *request) {
    request->send(409, "text/plain", "Shift to P (park) before updating the firmware");
  };
  auto not_parked = [](AsyncWebServerRequest *) { return !bridge_is_parked(); };
  s_server.on("/ota/start", HTTP_GET, refuse_ota).setFilter(not_parked);
  s_server.on("/ota/upload", HTTP_POST, refuse_ota).setFilter(not_parked);
  ElegantOTA.begin(&s_server, OTA_USERNAME, OTA_PASSWORD);

  s_server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain", "Not found"); });
  s_server.begin();
}

void web_ui_loop() {
  ElegantOTA.loop();
  static uint32_t last_cleanup = 0;
  if (millis() - last_cleanup >= 1000) {
    last_cleanup = millis();
    s_ws.cleanupClients();
  }
}
