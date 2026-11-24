/*
 * web_server.h - web UI, REST API, live WebSocket and firmware update.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Pages (gzip, ETag, strict Content-Security-Policy)
 *   GET  /, /app.js, /app.css
 *
 * Read-only API (no login)
 *   GET  /api/status        live bridge status
 *   GET  /api/settings      settings with their options and current values
 *   GET  /api/session       {"authenticated", "default_password", ...}
 *   GET  /api/network       access point name / state (never the password)
 *   GET  /api/system        diagnostics: chip, firmware, partitions, memory, tasks
 *   WS   /ws                {"type":"status","data":{...}} four times a second,
 *                           {"type":"settings"} after a change
 *
 * Login
 *   POST /api/login         {"password"} -> {"token"}; 401 wrong, 429 locked out
 *   POST /api/logout
 *
 * Changes - need "Authorization: Bearer <token>" (or HTTP basic auth with the
 * admin password, for scripts). While the factory default password is in use
 * every change except /api/password answers 403 password_change_required.
 *   POST /api/password      {"current", "new"} -> new token
 *   POST /api/settings      {"Inverter": "160", ...}           P only
 *   POST /api/network       {"ssid", "password"}
 *   POST /api/update        raw .bin (alias POST /update)       P only
 *   POST /api/reboot                                             P only
 *   POST /api/factory-reset                                      P only
 *   GET  /api/coredump      stored crash dump (404 if none)
 *   DELETE /api/coredump
 *
 * Errors are JSON: {"ok": false, "error": "<code>", "message": "<text>"}.
 */
#pragma once

#include "esp_err.h"

esp_err_t web_server_start(void);
