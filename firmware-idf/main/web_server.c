/*
 * web_server.c - web UI, REST API, live WebSocket and firmware update.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * All handlers run in the single esp_http_server task (CPU 0), so the shared
 * JSON buffer below needs no locking.
 */
#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "bridge_settings.h"
#include "json_util.h"
#include "network.h"
#include "security.h"
#include "sysinfo.h"

static const char *TAG = "web";

#define MAX_BODY          1024
#define OTA_CHUNK         4096
#define JSON_BUF          4096
#define MAX_CLIENT_FDS    16
#define WS_PUSH_PERIOD_US (250 * 1000)
#define RESTART_DELAY_US  (1000 * 1000)

static httpd_handle_t s_server;
static char s_etag[24];
static char s_json[JSON_BUF]; /* used from the HTTP server task only */
static esp_timer_handle_t s_ws_timer;
static esp_timer_handle_t s_restart_timer;
static volatile bool s_ws_push_queued;

/* ---- embedded files (gzip-compressed at build time, see main/CMakeLists.txt) --------- */
#define EMBEDDED(name)                                                   \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_end")

EMBEDDED(index_html_gz);
EMBEDDED(app_js_gz);
EMBEDDED(app_css_gz);

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *type;
    bool html;
} asset_t;

static const asset_t k_index = {index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8", true};
static const asset_t k_app_js = {app_js_gz_start, app_js_gz_end, "text/javascript; charset=utf-8", false};
static const asset_t k_app_css = {app_css_gz_start, app_css_gz_end, "text/css; charset=utf-8", false};

/* ---- response helpers ------------------------------------------------------------------ */
static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static esp_err_t send_json_str(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_common_headers(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_writer(httpd_req_t *req, const char *status, json_writer_t *w)
{
    if (!jw_ok(w)) {
        ESP_LOGE(TAG, "JSON response for %s did not fit", req->uri);
        return send_json_str(req, "500 Internal Server Error",
                             "{\"ok\":false,\"error\":\"internal\",\"message\":\"Response too large\"}");
    }
    return send_json_str(req, status, w->buf);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
    char buf[320];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", false);
    jw_kv_str(&w, "error", code);
    jw_kv_str(&w, "message", message);
    jw_obj_end(&w);
    return send_writer(req, status, &w);
}

static esp_err_t send_ok(httpd_req_t *req, const char *message)
{
    char buf[320];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", true);
    jw_kv_str(&w, "message", message);
    jw_obj_end(&w);
    return send_writer(req, "200 OK", &w);
}

/* Reads the request body (at most `cap - 1` bytes, NUL terminated). Returns the
 * length, or -1 after sending an error response. */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    const size_t total = req->content_len;
    if (total >= cap) {
        send_error(req, "413 Content Too Large", "too_large", "Request body too large");
        return -1;
    }
    size_t off = 0;
    int timeouts = 0;
    while (off < total) {
        const int r = httpd_req_recv(req, buf + off, total - off);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            send_error(req, "408 Request Timeout", "timeout", "Request body incomplete");
            return -1;
        }
        off += (size_t)r;
    }
    buf[off] = '\0';
    return (int)off;
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_later(void)
{
    if (s_restart_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "restart"};
        if (esp_timer_create(&args, &s_restart_timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_US);
}

/* ---- authentication ------------------------------------------------------------------- */
static int b64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

/* Strict base64 decoder. Returns the decoded length or -1. */
static int base64_decode(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    const char *p = in;
    for (; *p != '\0' && *p != '='; p++) {
        const int v = b64_value(*p);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= cap) {
                return -1;
            }
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    while (*p == '=') {
        p++;
    }
    if (*p != '\0') {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

typedef enum {
    AUTH_NONE,      /* no or invalid credentials */
    AUTH_OK,
    AUTH_LOCKED,
} auth_result_t;

static auth_result_t check_auth(httpd_req_t *req, uint32_t *retry_after)
{
    char hdr[160];
    *retry_after = 0;
    const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len >= sizeof(hdr) || httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return AUTH_NONE;
    }
    if (strncmp(hdr, "Bearer ", 7) == 0) {
        return security_session_valid(hdr + 7) ? AUTH_OK : AUTH_NONE;
    }
    if (strncmp(hdr, "Basic ", 6) == 0) {
        char plain[128];
        if (base64_decode(hdr + 6, plain, sizeof(plain)) < 0) {
            return AUTH_NONE;
        }
        char *colon = strchr(plain, ':'); /* the user name is not checked */
        if (colon == NULL) {
            return AUTH_NONE;
        }
        const security_login_result_t r = security_verify_password(colon + 1, retry_after);
        memset(plain, 0, sizeof(plain));
        return (r == SECURITY_LOGIN_OK) ? AUTH_OK : (r == SECURITY_LOGIN_LOCKED ? AUTH_LOCKED : AUTH_NONE);
    }
    return AUTH_NONE;
}

static void send_locked(httpd_req_t *req, uint32_t retry_after)
{
    char secs[12], msg[96];
    snprintf(secs, sizeof(secs), "%lu", (unsigned long)retry_after);
    snprintf(msg, sizeof(msg), "Too many wrong passwords. Try again in %lu s.", (unsigned long)retry_after);
    httpd_resp_set_hdr(req, "Retry-After", secs);
    send_error(req, "429 Too Many Requests", "locked", msg);
}

/* Sends the error response and returns false unless the request is authorized.
 * With allow_default == false the factory default password is not enough. */
static bool require_auth(httpd_req_t *req, bool allow_default)
{
    uint32_t retry_after = 0;
    const auth_result_t r = check_auth(req, &retry_after);
    if (r == AUTH_LOCKED) {
        send_locked(req, retry_after);
        return false;
    }
    if (r != AUTH_OK) {
        send_error(req, "401 Unauthorized", "unauthorized", "Log in with the admin password first");
        return false;
    }
    if (!allow_default && security_is_default_password()) {
        send_error(req, "403 Forbidden", "password_change_required",
                   "Set your own admin password before making changes");
        return false;
    }
    return true;
}

static bool require_parked(httpd_req_t *req, const char *action)
{
    if (bridge_is_parked()) {
        return true;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Shift to P (park) before %s", action);
    send_error(req, "409 Conflict", "not_parked", msg);
    return false;
}

/* ---- JSON documents ------------------------------------------------------------------------ */
static void write_bus(json_writer_t *w, const char *key, const bridge_bus_stats_t *b)
{
    jw_key(w, key);
    jw_obj_begin(w);
    jw_kv_bool(w, "running", b->running);
    jw_kv_str(w, "state", bridge_bus_state_name(b->running ? b->state : BRIDGE_BUS_STOPPED));
    jw_kv_uint(w, "rx", b->rx);
    jw_kv_uint(w, "tx", b->tx);
    jw_kv_uint(w, "rx_per_s", b->rx_per_s);
    jw_kv_uint(w, "tx_per_s", b->tx_per_s);
    jw_kv_uint(w, "tx_dropped", b->tx_dropped);
    jw_kv_uint(w, "rx_overflow", b->rx_overflow);
    jw_kv_uint(w, "bus_off", b->bus_off);
    jw_kv_uint(w, "restarts", b->restarts);
    jw_kv_uint(w, "tec", b->tec);
    jw_kv_uint(w, "rec", b->rec);
    jw_obj_end(w);
}

static void write_multipliers(json_writer_t *w, const leaf_inverter_config_t *cfg)
{
    jw_kv_fixed(w, "drive_multiplier", cfg->modify_torque ? cfg->drive_torque_permille : 1000, 3);
    jw_kv_fixed(w, "regen_multiplier", (cfg->modify_torque && cfg->regen_tuning) ? cfg->regen_torque_permille : 1000,
                3);
}

static void write_status(json_writer_t *w)
{
    bridge_status_t st;
    bridge_get_status(&st);
    /* Mode and multipliers come from the same snapshot: what the bridge task has
     * applied, not what was just saved (that takes effect within ~100 ms). */
    const uint8_t inv = st.applied.value[BRIDGE_SETTING_INVERTER];

    jw_obj_begin(w);
    jw_kv_str(w, "firmware", esp_app_get_description()->version);
    jw_kv_str(w, "leaf_can", LEAF_CAN_VERSION_STRING);
    jw_kv_uint(w, "uptime_s", st.uptime_s);
    jw_kv_str(w, "shifter", leaf_shifter_name(st.shifter));
    jw_kv_bool(w, "parked", st.shifter == LEAF_SHIFTER_PARK);

    jw_key(w, "inverter");
    jw_obj_begin(w);
    jw_kv_str(w, "mode", bridge_setting_option_value(BRIDGE_SETTING_INVERTER, inv));
    jw_kv_str(w, "label", bridge_setting_option_label(BRIDGE_SETTING_INVERTER, inv));
    jw_kv_bool(w, "modify_torque", st.config.modify_torque);
    write_multipliers(w, &st.config);
    jw_obj_end(w);

    jw_key(w, "torque");
    jw_obj_begin(w);
    jw_kv_fixed(w, "vcm_request_nm", (long long)st.torque_request_vcm * 25, 2);  /* 0.25 Nm */
    jw_kv_fixed(w, "out_request_nm", (long long)st.torque_request_out * 25, 2);
    jw_kv_fixed(w, "response_nm", (long long)st.torque_response * 5, 1);          /* 0.5 Nm */
    jw_obj_end(w);

    jw_key(w, "battery");
    jw_obj_begin(w);
    jw_key(w, "soc_percent");
    if (st.soc_valid) {
        jw_fixed(w, st.soc_permille, 1);
    } else {
        jw_null(w);
    }
    jw_kv_uint(w, "charge_status", st.charge_status);
    jw_kv_bool(w, "ac_charging", st.ac_charging);
    jw_obj_end(w);

    jw_key(w, "bridge");
    jw_obj_begin(w);
    jw_kv_uint(w, "frames_from_vcm", st.inverter.frames_in[LEAF_BUS_VCM]);
    jw_kv_uint(w, "frames_from_inverter", st.inverter.frames_in[LEAF_BUS_INVERTER]);
    jw_kv_uint(w, "frames_modified", st.inverter.frames_modified);
    jw_kv_uint(w, "frames_generated", st.inverter.frames_generated);
    jw_kv_uint(w, "torque_clamped", st.inverter.torque_clamped);
    jw_kv_uint(w, "queue_overflow", st.queue_overflow);
    jw_kv_uint(w, "queue_high_water", st.queue_high_water);
    jw_kv_uint(w, "queue_depth", st.queue_depth);
    jw_kv_uint(w, "latency_us_avg", st.latency_us_avg);
    jw_kv_uint(w, "latency_us_max", st.latency_us_max);
    jw_kv_uint(w, "latency_us_peak", st.latency_us_peak);
    jw_obj_end(w);

    jw_key(w, "buses");
    jw_obj_begin(w);
    write_bus(w, "vcm", &st.bus[LEAF_BUS_VCM]);
    write_bus(w, "inverter", &st.bus[LEAF_BUS_INVERTER]);
    jw_obj_end(w);

    jw_key(w, "wifi");
    jw_obj_begin(w);
    jw_kv_int(w, "clients", network_client_count());
    jw_obj_end(w);

    jw_key(w, "security");
    jw_obj_begin(w);
    jw_kv_bool(w, "default_password", security_is_default_password());
    jw_kv_bool(w, "default_wifi_password", security_is_default_wifi_password());
    jw_obj_end(w);

    jw_obj_end(w);
}

/* ---- pages ------------------------------------------------------------------------------------ */
static esp_err_t asset_handler(httpd_req_t *req)
{
    const asset_t *a = (const asset_t *)req->user_ctx;
    char inm[sizeof(s_etag) + 8];
    set_common_headers(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, s_etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    if (a->html) {
        httpd_resp_set_hdr(req, "Content-Security-Policy",
                           "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                           "connect-src 'self' ws: wss:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

static esp_err_t update_page_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/#firmware");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- read-only API ------------------------------------------------------------------------------ */
static esp_err_t status_handler(httpd_req_t *req)
{
    json_writer_t w;
    jw_init(&w, s_json, sizeof(s_json));
    write_status(&w);
    return send_writer(req, "200 OK", &w);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    bridge_settings_t s;
    bridge_get_settings(&s);
    json_writer_t w;
    jw_init(&w, s_json, sizeof(s_json));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "parked", bridge_is_parked());
    jw_key(&w, "settings");
    jw_arr_begin(&w);
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        const bridge_setting_id_t id = (bridge_setting_id_t)i;
        jw_obj_begin(&w);
        jw_kv_str(&w, "id", bridge_setting_form_name(id));
        jw_kv_bool(&w, "implemented", bridge_setting_is_implemented(id));
        jw_kv_str(&w, "value", bridge_setting_option_value(id, s.value[i]));
        jw_key(&w, "options");
        jw_arr_begin(&w);
        for (uint8_t o = 0; o < bridge_setting_option_count(id); o++) {
            jw_obj_begin(&w);
            jw_kv_str(&w, "value", bridge_setting_option_value(id, o));
            jw_kv_str(&w, "label", bridge_setting_option_label(id, o));
            if (id == BRIDGE_SETTING_INVERTER) {
                leaf_inverter_config_t cfg;
                leaf_inverter_default_config(&cfg, (leaf_inverter_upgrade_t)o);
                write_multipliers(&w, &cfg);
            }
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    return send_writer(req, "200 OK", &w);
}

static esp_err_t session_handler(httpd_req_t *req)
{
    uint32_t retry = 0;
    char hdr[48];
    bool authenticated = false;
    /* Only bearer tokens are checked here: no password hashing on a status probe. */
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0) {
        authenticated = check_auth(req, &retry) == AUTH_OK;
    }
    char buf[160];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "authenticated", authenticated);
    jw_kv_bool(&w, "default_password", security_is_default_password());
    jw_kv_bool(&w, "default_wifi_password", security_is_default_wifi_password());
    jw_obj_end(&w);
    return send_writer(req, "200 OK", &w);
}

static esp_err_t network_get_handler(httpd_req_t *req)
{
    char ssid[SECURITY_WIFI_SSID_MAX + 1];
    security_get_wifi(ssid, sizeof(ssid), NULL, 0);
    char buf[200];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_str(&w, "ssid", ssid);
    jw_kv_bool(&w, "default_password", security_is_default_wifi_password());
    jw_kv_int(&w, "channel", network_channel());
    jw_kv_int(&w, "clients", network_client_count());
    jw_obj_end(&w);
    return send_writer(req, "200 OK", &w);
}

static esp_err_t system_handler(httpd_req_t *req)
{
    json_writer_t w;
    jw_init(&w, s_json, sizeof(s_json));
    sysinfo_write_json(&w);
    return send_writer(req, "200 OK", &w);
}

/* ---- login / password -------------------------------------------------------------------------------- */
static esp_err_t send_token(httpd_req_t *req, const char *token, const char *message)
{
    char buf[256];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", true);
    jw_kv_str(&w, "token", token);
    jw_kv_uint(&w, "idle_timeout_s", SECURITY_SESSION_IDLE_S);
    jw_kv_bool(&w, "default_password", security_is_default_password());
    if (message != NULL) {
        jw_kv_str(&w, "message", message);
    }
    jw_obj_end(&w);
    return send_writer(req, "200 OK", &w);
}

static esp_err_t login_handler(httpd_req_t *req)
{
    char body[256];
    char password[SECURITY_PASSWORD_MAX * 2 + 1];
    const int len = read_body(req, body, sizeof(body));
    if (len < 0) {
        return ESP_OK;
    }
    if (json_flat_get_string(body, (size_t)len, "password", password, sizeof(password)) != JSON_FIELD_OK) {
        return send_error(req, "400 Bad Request", "bad_request", "Expected {\"password\": \"...\"}");
    }
    char token[SECURITY_TOKEN_LEN + 1];
    uint32_t retry = 0;
    const security_login_result_t r = security_login(password, token, &retry);
    memset(password, 0, sizeof(password));
    if (r == SECURITY_LOGIN_LOCKED) {
        send_locked(req, retry);
        return ESP_OK;
    }
    if (r != SECURITY_LOGIN_OK) {
        if (retry > 0) {
            send_locked(req, retry);
            return ESP_OK;
        }
        return send_error(req, "401 Unauthorized", "bad_password", "Wrong password");
    }
    ESP_LOGI(TAG, "login");
    return send_token(req, token, NULL);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    char hdr[48];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0) {
        security_logout(hdr + 7);
    }
    return send_ok(req, "Logged out");
}

static esp_err_t password_handler(httpd_req_t *req)
{
    if (!require_auth(req, true)) {
        return ESP_OK;
    }
    char body[512];
    char current[SECURITY_PASSWORD_MAX * 2 + 1], next[SECURITY_PASSWORD_MAX * 2 + 1];
    const int len = read_body(req, body, sizeof(body));
    if (len < 0) {
        return ESP_OK;
    }
    if (json_flat_get_string(body, (size_t)len, "current", current, sizeof(current)) != JSON_FIELD_OK ||
        json_flat_get_string(body, (size_t)len, "new", next, sizeof(next)) != JSON_FIELD_OK) {
        memset(body, 0, sizeof(body));
        return send_error(req, "400 Bad Request", "bad_request", "Expected {\"current\": \"...\", \"new\": \"...\"}");
    }
    memset(body, 0, sizeof(body));
    uint32_t retry = 0;
    const security_login_result_t r = security_verify_password(current, &retry);
    memset(current, 0, sizeof(current));
    if (r != SECURITY_LOGIN_OK) {
        memset(next, 0, sizeof(next));
        if (r == SECURITY_LOGIN_LOCKED || retry > 0) {
            send_locked(req, retry);
            return ESP_OK;
        }
        return send_error(req, "403 Forbidden", "bad_password", "The current password is wrong");
    }
    const char *problem = security_check_new_password(next);
    if (problem != NULL) {
        memset(next, 0, sizeof(next));
        return send_error(req, "400 Bad Request", "weak_password", problem);
    }
    if (security_set_admin_password(next) != ESP_OK) {
        memset(next, 0, sizeof(next));
        return send_error(req, "500 Internal Server Error", "storage", "Could not store the new password");
    }
    /* Sign every other session out and hand this client a fresh token. */
    security_logout_all();
    char token[SECURITY_TOKEN_LEN + 1];
    const security_login_result_t lr = security_login(next, token, &retry);
    memset(next, 0, sizeof(next));
    if (lr != SECURITY_LOGIN_OK) {
        return send_ok(req, "Password changed - log in again");
    }
    return send_token(req, token, "Password changed");
}

/* ---- changes ------------------------------------------------------------------------------------------- */
static void ws_broadcast(const char *text);

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!require_auth(req, false)) {
        return ESP_OK;
    }
    char body[MAX_BODY + 1];
    const int len = read_body(req, body, sizeof(body));
    if (len < 0) {
        return ESP_OK;
    }
    bridge_settings_t current, next;
    char err[96];
    bridge_get_settings(&current);
    if (!bridge_settings_parse_json(body, (size_t)len, &current, &next, err, sizeof(err))) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        return send_error(req, "400 Bad Request", "invalid_settings", msg);
    }
    switch (bridge_request_settings(&next)) {
    case BRIDGE_SETTINGS_SAVED:
        ESP_LOGI(TAG, "settings saved");
        ws_broadcast("{\"type\":\"settings\"}");
        return send_ok(req, "Settings saved and applied");
    case BRIDGE_SETTINGS_UNCHANGED:
        return send_ok(req, "No changes");
    case BRIDGE_SETTINGS_NOT_PARKED:
        return send_error(req, "409 Conflict", "not_parked", "Shift to P (park) before changing settings");
    case BRIDGE_SETTINGS_STORAGE_ERROR:
    default:
        return send_error(req, "500 Internal Server Error", "storage", "Could not write the settings to flash");
    }
}

static esp_err_t network_post_handler(httpd_req_t *req)
{
    if (!require_auth(req, false)) {
        return ESP_OK;
    }
    char body[512];
    char ssid[SECURITY_WIFI_SSID_MAX * 2 + 1], password[SECURITY_WIFI_PASSWORD_MAX * 2 + 1];
    const int len = read_body(req, body, sizeof(body));
    if (len < 0) {
        return ESP_OK;
    }
    if (json_flat_get_string(body, (size_t)len, "ssid", ssid, sizeof(ssid)) != JSON_FIELD_OK ||
        json_flat_get_string(body, (size_t)len, "password", password, sizeof(password)) != JSON_FIELD_OK) {
        memset(body, 0, sizeof(body));
        return send_error(req, "400 Bad Request", "bad_request", "Expected {\"ssid\": \"...\", \"password\": \"...\"}");
    }
    memset(body, 0, sizeof(body));
    const char *problem = security_check_wifi(ssid, password);
    if (problem != NULL) {
        memset(password, 0, sizeof(password));
        return send_error(req, "400 Bad Request", "invalid_network", problem);
    }
    const esp_err_t err = security_set_wifi(ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "storage", "Could not store the Wi-Fi settings");
    }
    network_apply_credentials_later(2000);
    char msg[200];
    snprintf(msg, sizeof(msg), "Saved. The access point restarts as \"%s\" - reconnect with the new password.", ssid);
    return send_ok(req, msg);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!require_auth(req, false) || !require_parked(req, "restarting the bridge")) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "restart requested");
    send_ok(req, "Restarting");
    restart_later();
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (!require_auth(req, false) || !require_parked(req, "a factory reset")) {
        return ESP_OK;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIDGE_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = security_erase();
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "storage", "Could not erase the stored configuration");
    }
    ESP_LOGW(TAG, "factory reset - restarting");
    send_ok(req, "Factory reset done - restarting with default settings and passwords");
    restart_later();
    return ESP_OK;
}

static esp_err_t coredump_get_handler(httpd_req_t *req)
{
    if (!require_auth(req, false)) {
        return ESP_OK;
    }
    size_t addr = 0, size = 0;
    if (!sysinfo_coredump_present(NULL) || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return send_error(req, "404 Not Found", "not_found", "No crash dump stored");
    }
    char *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "no_memory", "Out of memory");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"leaf-bridge-coredump.bin\"");
    set_common_headers(req);
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < size && err == ESP_OK; off += OTA_CHUNK) {
        const size_t n = (size - off < OTA_CHUNK) ? size - off : OTA_CHUNK;
        err = esp_flash_read(NULL, buf, addr + off, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        }
    }
    free(buf);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t coredump_delete_handler(httpd_req_t *req)
{
    if (!require_auth(req, false)) {
        return ESP_OK;
    }
    if (esp_core_dump_image_erase() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "storage", "Could not erase the crash dump");
    }
    return send_ok(req, "Crash dump erased");
}

/* ---- firmware update ---------------------------------------------------------------------------------------- */
#define IMAGE_PREFIX_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* Checks the start of an uploaded image. Returns NULL if acceptable. */
static const char *check_image(const uint8_t *data, char *version, size_t version_len, char *why, size_t why_len)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)data;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "This is not an ESP32 application image (.bin)";
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "This image was built for a different chip";
    }
    const esp_app_desc_t *own = esp_app_get_description();
    if (strncmp(desc->project_name, own->project_name, sizeof(desc->project_name)) != 0) {
        snprintf(why, why_len, "This image is a different firmware (\"%.32s\"), not %s", desc->project_name,
                 own->project_name);
        return why;
    }
    snprintf(version, version_len, "%.32s", desc->version);
    return NULL;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!require_auth(req, false) || !require_parked(req, "updating the firmware")) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return send_error(req, "500 Internal Server Error", "no_partition", "No OTA partition");
    }
    const size_t total = req->content_len;
    if (total < IMAGE_PREFIX_LEN) {
        return send_error(req, "400 Bad Request", "invalid_image", "This is not an ESP32 application image (.bin)");
    }
    if (total > part->size) {
        return send_error(req, "413 Content Too Large", "too_large", "The image is larger than the update slot");
    }
    uint8_t *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "no_memory", "Out of memory");
    }

    /* Read and check the image header and app description before touching flash. */
    size_t have = 0;
    int timeouts = 0;
    while (have < IMAGE_PREFIX_LEN) {
        const int r = httpd_req_recv(req, (char *)buf + have, OTA_CHUNK - have);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return send_error(req, "408 Request Timeout", "timeout", "Upload interrupted");
        }
        have += (size_t)r;
    }
    char version[33] = "", why[128];
    const char *problem = check_image(buf, version, sizeof(version), why, sizeof(why));
    if (problem != NULL) {
        free(buf);
        ESP_LOGW(TAG, "firmware update rejected: %s", problem);
        return send_error(req, "400 Bad Request", "invalid_image", problem);
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        free(buf);
        return send_error(req, "500 Internal Server Error", "ota_begin", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "firmware update to %s: %u bytes into %s", version, (unsigned)total, part->label);

    const char *fail = NULL;
    size_t received = 0;
    size_t pending = have;
    timeouts = 0;
    for (;;) {
        if (pending > 0) {
            if (!bridge_is_parked()) {
                fail = "The car left P during the update - aborted, the current firmware stays";
                break;
            }
            if (esp_ota_write(ota, buf, pending) != ESP_OK) {
                fail = "The image is corrupt";
                break;
            }
            received += pending;
            pending = 0;
        }
        if (received >= total) {
            break;
        }
        const size_t want = (total - received) < OTA_CHUNK ? (total - received) : OTA_CHUNK;
        const int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            fail = "Upload interrupted - the current firmware stays";
            break;
        }
        timeouts = 0;
        pending = (size_t)r;
    }
    free(buf);

    if (fail != NULL) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "firmware update failed: %s", fail);
        return send_error(req, "400 Bad Request", "update_failed", fail);
    }
    err = esp_ota_end(ota); /* verifies the image checksum / SHA-256 */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware update rejected: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", "verification_failed",
                          "Image verification failed - the current firmware stays");
    }

    ESP_LOGW(TAG, "firmware update complete, rebooting into %s", version);
    char out[256];
    json_writer_t w;
    jw_init(&w, out, sizeof(out));
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", true);
    jw_kv_str(&w, "version", version);
    jw_kv_str(&w, "message", "Update installed - the bridge restarts now");
    jw_obj_end(&w);
    send_writer(req, "200 OK", &w);
    restart_later();
    return ESP_OK;
}

/* ---- WebSocket ------------------------------------------------------------------------------------------------ */
static void ws_send_to_all(const char *text, int only_fd)
{
    int fds[MAX_CLIENT_FDS];
    size_t n = MAX_CLIENT_FDS;
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return;
    }
    httpd_ws_frame_t pkt = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    for (size_t i = 0; i < n; i++) {
        if ((only_fd < 0 || fds[i] == only_fd) && httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            (void)httpd_ws_send_frame_async(s_server, fds[i], &pkt);
        }
    }
}

static bool build_status_message(void)
{
    json_writer_t w;
    jw_init(&w, s_json, sizeof(s_json));
    jw_obj_begin(&w);
    jw_kv_str(&w, "type", "status");
    jw_key(&w, "data");
    write_status(&w);
    jw_obj_end(&w);
    return jw_ok(&w);
}

static bool any_ws_client(void)
{
    int fds[MAX_CLIENT_FDS];
    size_t n = MAX_CLIENT_FDS;
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            return true;
        }
    }
    return false;
}

/* Runs in the server task: arg < 0 -> periodic push to every client, else one fd. */
static void ws_work_push_status(void *arg)
{
    const int only_fd = (int)(intptr_t)arg;
    if (only_fd < 0) {
        s_ws_push_queued = false;
        if (!any_ws_client()) {
            return;
        }
    }
    if (build_status_message()) {
        ws_send_to_all(s_json, only_fd);
    }
}

static void ws_work_broadcast_text(void *arg)
{
    ws_send_to_all((const char *)arg, -1);
}

static void ws_broadcast(const char *text)
{
    /* `text` must be a string literal: it is used later from the server task. */
    (void)httpd_queue_work(s_server, ws_work_broadcast_text, (void *)text);
}

static void ws_timer_cb(void *arg)
{
    (void)arg;
    if (!s_ws_push_queued) {
        s_ws_push_queued = true;
        if (httpd_queue_work(s_server, ws_work_push_status, (void *)(intptr_t)-1) != ESP_OK) {
            s_ws_push_queued = false;
        }
    }
}

static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    return httpd_queue_work(s_server, ws_work_push_status, (void *)(intptr_t)httpd_req_to_sockfd(req));
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    /* Clients never need to send anything; read and discard. */
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0) {
        return err;
    }
    if (frame.len > 128) {
        return ESP_FAIL; /* closes the connection */
    }
    uint8_t buf[129];
    frame.payload = buf;
    return httpd_ws_recv_frame(req, &frame, frame.len);
}

/* ---- captive portal / 404 ----------------------------------------------------------------------------------- */
static bool host_is_ip_literal(const char *host)
{
    if (host[0] == '[') {
        return true; /* IPv6 literal */
    }
    for (const char *p = host; *p != '\0' && *p != ':'; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') {
            return false;
        }
    }
    return true;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
#if CONFIG_BRIDGE_CAPTIVE_PORTAL
    char host[96];
    uint8_t ip[4];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && !host_is_ip_literal(host) &&
        network_get_ip(ip)) {
        /* A name we answered in DNS, e.g. connectivitycheck.gstatic.com/generate_204:
         * send the phone to the configuration page ("Sign in to network"). */
        char location[40];
        snprintf(location, sizeof(location), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
#endif
    if (strncmp(req->uri, "/api/", 5) == 0) {
        return send_error(req, "404 Not Found", "not_found", "Unknown API endpoint");
    }
    set_common_headers(req);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Not found");
}

/* ---- start ----------------------------------------------------------------------------------------------------- */
esp_err_t web_server_start(void)
{
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    snprintf(s_etag, sizeof(s_etag), "\"%s\"", sha);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 24;
    cfg.max_resp_headers = 12;
    cfg.max_open_sockets = 7;
    cfg.stack_size = 8192;
    cfg.core_id = 0; /* keep CPU 1 for the bridge */
    cfg.task_priority = tskIDLE_PRIORITY + 5;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    /* A phone that stops reading (screen off) must not stall the single server
     * task for long: its WebSocket send fails after 5 s and the socket is closed. */
    cfg.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&k_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&k_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&k_app_js},
        {.uri = "/app.css", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&k_app_css},
        {.uri = "/update", .method = HTTP_GET, .handler = update_page_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler},
        {.uri = "/api/session", .method = HTTP_GET, .handler = session_handler},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_handler},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_handler},
        {.uri = "/api/network", .method = HTTP_GET, .handler = network_get_handler},
        {.uri = "/api/network", .method = HTTP_POST, .handler = network_post_handler},
        {.uri = "/api/system", .method = HTTP_GET, .handler = system_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = coredump_get_handler},
        {.uri = "/api/coredump", .method = HTTP_DELETE, .handler = coredump_delete_handler},
        {.uri = "/api/update", .method = HTTP_POST, .handler = ota_handler},
        {.uri = "/update", .method = HTTP_POST, .handler = ota_handler},
        {.uri = "/ws",
         .method = HTTP_GET,
         .handler = ws_handler,
         .is_websocket = true,
         .ws_post_handshake_cb = ws_post_handshake},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler));

    const esp_timer_create_args_t args = {.callback = ws_timer_cb, .name = "ws_push"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_ws_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ws_timer, WS_PUSH_PERIOD_US));

    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
