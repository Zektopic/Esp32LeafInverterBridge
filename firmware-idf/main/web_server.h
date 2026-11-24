/*
 * web_server.h - configuration page, status API, WebSocket and OTA update.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 *   GET  /             configuration page (embedded web/index.html)
 *   GET  /static/...   page assets (embedded)
 *   POST /settings     JSON body; 200 / 400 / 409 / 500 with a plain-text message
 *   GET  /api/status   live bridge status as JSON
 *   WS   /ws           pushes the selected radio button ids (e.g. "Inverter2")
 *   GET  /update       firmware update page        (HTTP basic auth)
 *   POST /update       raw .bin upload, P only      (HTTP basic auth)
 */
#pragma once

#include "esp_err.h"

esp_err_t web_server_start(void);
