/*
 * web_server.c - configuration page, status API, WebSocket and OTA update.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "bridge_settings.h"

static const char *TAG = "web";

#define MAX_SETTINGS_BODY 1024
#define OTA_CHUNK         4096
#define MAX_WS_CLIENTS    CONFIG_LWIP_MAX_SOCKETS

static httpd_handle_t s_server;
static char s_expected_auth[128]; /* "Basic base64(user:password)" */

/* ---- embedded files ----------------------------------------------------------------- */
#define EMBEDDED(name)                                                   \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_end")

/* gzip-compressed at build time, see main/CMakeLists.txt */
EMBEDDED(index_html_gz);
EMBEDDED(update_html_gz);
EMBEDDED(bootstrap_min_css_gz);
EMBEDDED(bootstrap_min_js_gz);
EMBEDDED(jquery_min_js_gz);
EMBEDDED(popper_min_js_gz);

typedef struct {
    const char *path;
    const uint8_t *start;
    const uint8_t *end;
    const char *type;
} asset_t;

static const asset_t k_static_assets[] = {
    {"/static/bootstrap.min.css", bootstrap_min_css_gz_start, bootstrap_min_css_gz_end, "text/css"},
    {"/static/bootstrap.min.js", bootstrap_min_js_gz_start, bootstrap_min_js_gz_end, "application/javascript"},
    {"/static/jquery.min.js", jquery_min_js_gz_start, jquery_min_js_gz_end, "application/javascript"},
    {"/static/popper.min.js", popper_min_js_gz_start, popper_min_js_gz_end, "application/javascript"},
};

/* EMBED_FILES appends nothing, so the length is end - start. Every asset is gzip. */
static esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *type,
                               const char *cache)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    if (cache != NULL) {
        httpd_resp_set_hdr(req, "Cache-Control", cache);
    }
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* ---- HTTP basic auth ------------------------------------------------------------------- */
static void base64_encode(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 4 < out_len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0u) |
                           (i + 2 < len ? (uint32_t)in[i + 2] : 0u);
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
}

static void init_auth(void)
{
    const char *password = (sizeof(CONFIG_BRIDGE_OTA_PASSWORD) > 1) ? CONFIG_BRIDGE_OTA_PASSWORD
                                                                    : CONFIG_BRIDGE_WIFI_PASSWORD;
    char plain[96];
    snprintf(plain, sizeof(plain), "%s:%s", CONFIG_BRIDGE_OTA_USERNAME, password);
    strcpy(s_expected_auth, "Basic ");
    base64_encode((const uint8_t *)plain, strlen(plain), s_expected_auth + 6, sizeof(s_expected_auth) - 6);
}

static bool authorized(httpd_req_t *req)
{
    char got[sizeof(s_expected_auth)] = {0};
    const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    bool ok = false;
    if (len > 0 && len < sizeof(got) && httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK) {
        /* constant-time compare */
        const size_t n = strlen(s_expected_auth);
        uint8_t diff = (uint8_t)(strlen(got) != n);
        for (size_t i = 0; i < n; i++) {
            diff |= (uint8_t)(got[i] ^ s_expected_auth[i]);
        }
        ok = (diff == 0);
    }
    if (!ok) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"LEAF inverter bridge\"");
        (void)send_text(req, "401 Unauthorized", "Authentication required");
    }
    return ok;
}

/* ---- WebSocket ------------------------------------------------------------------------- */
static void ws_send_options_to(int fd)
{
    bridge_settings_t s;
    char id[32];
    bridge_get_settings(&s);
    for (int i = 0; i < BRIDGE_SETTING_COUNT; i++) {
        if (bridge_settings_radio_id(&s, (bridge_setting_id_t)i, id, sizeof(id))) {
            httpd_ws_frame_t pkt = {
                .final = true,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)id,
                .len = strlen(id),
            };
            if (httpd_ws_send_frame_async(s_server, fd, &pkt) != ESP_OK) {
                return;
            }
        }
    }
}

static void ws_work_send_one(void *arg)
{
    ws_send_options_to((int)(intptr_t)arg);
}

static void ws_work_broadcast(void *arg)
{
    (void)arg;
    int fds[MAX_WS_CLIENTS];
    size_t n = MAX_WS_CLIENTS;
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            ws_send_options_to(fds[i]);
        }
    }
}

/* Called once the 101 response has been sent: push the current selection. */
static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    return httpd_queue_work(s_server, ws_work_send_one, (void *)(intptr_t)httpd_req_to_sockfd(req));
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    /* The page never sends anything meaningful; read and discard. */
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

/* ---- pages ------------------------------------------------------------------------------- */
static esp_err_t index_handler(httpd_req_t *req)
{
    return send_embedded(req, index_html_gz_start, index_html_gz_end, "text/html", "no-cache");
}

static esp_err_t static_handler(httpd_req_t *req)
{
    for (size_t i = 0; i < sizeof(k_static_assets) / sizeof(k_static_assets[0]); i++) {
        if (strcmp(req->uri, k_static_assets[i].path) == 0) {
            return send_embedded(req, k_static_assets[i].start, k_static_assets[i].end, k_static_assets[i].type,
                                 "max-age=86400");
        }
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

/* ---- POST /settings ------------------------------------------------------------------------ */
static esp_err_t settings_handler(httpd_req_t *req)
{
    char body[MAX_SETTINGS_BODY + 1];
    const int total = (int)req->content_len;
    if (total <= 0 || total > MAX_SETTINGS_BODY) {
        return send_text(req, "400 Bad Request", "Missing or too large settings body");
    }
    int off = 0;
    while (off < total) {
        const int r = httpd_req_recv(req, body + off, (size_t)(total - off));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        off += r;
    }
    body[off] = '\0';

    bridge_settings_t current, next;
    char err[96];
    bridge_get_settings(&current);
    if (!bridge_settings_parse_json(body, (size_t)off, &current, &next, err, sizeof(err))) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Rejected: %s", err);
        return send_text(req, "400 Bad Request", msg);
    }

    switch (bridge_request_settings(&next)) {
    case BRIDGE_SETTINGS_SAVED:
        ESP_LOGI(TAG, "settings saved");
        (void)httpd_queue_work(s_server, ws_work_broadcast, NULL);
        return send_text(req, "200 OK", "Settings saved");
    case BRIDGE_SETTINGS_UNCHANGED:
        return send_text(req, "200 OK", "Settings unchanged");
    case BRIDGE_SETTINGS_NOT_PARKED:
        return send_text(req, "409 Conflict", "Shift to P (park) before changing settings");
    case BRIDGE_SETTINGS_STORAGE_ERROR:
    default:
        return send_text(req, "500 Internal Server Error", "Could not write settings to flash");
    }
}

/* ---- GET /api/status ------------------------------------------------------------------------ */
static esp_err_t status_handler(httpd_req_t *req)
{
    bridge_status_t st;
    bridge_settings_t s;
    bridge_get_status(&st);
    bridge_get_settings(&s);
    const bridge_bus_stats_t *vcm = &st.bus[LEAF_BUS_VCM];
    const bridge_bus_stats_t *inv = &st.bus[LEAF_BUS_INVERTER];

    char soc[8] = "null";
    if (st.soc_valid) {
        snprintf(soc, sizeof(soc), "%u.%u", st.soc_permille / 10u, st.soc_permille % 10u);
    }
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"firmware\":\"%s\",\"leaf_can\":\"%s\",\"uptime_s\":%lu,"
             "\"inverter_setting\":\"%s\",\"shifter\":\"%s\","
             "\"torque_request_vcm_nm\":%.2f,\"torque_request_out_nm\":%.2f,\"torque_response_nm\":%.1f,"
             "\"charge_status\":%u,\"ac_charging\":%s,\"soc_percent\":%s,"
             "\"frames_modified\":%lu,\"frames_generated\":%lu,\"torque_clamped\":%lu,"
             "\"queue_overflow\":%lu,\"queue_high_water\":%lu,"
             "\"vcm_bus\":{\"running\":%s,\"rx\":%lu,\"tx\":%lu,\"tx_dropped\":%lu,\"rx_overflow\":%lu,\"bus_off\":%lu},"
             "\"inverter_bus\":{\"running\":%s,\"rx\":%lu,\"tx\":%lu,\"tx_dropped\":%lu,\"rx_overflow\":%lu,\"bus_off\":%lu}}",
             esp_app_get_description()->version, LEAF_CAN_VERSION_STRING, (unsigned long)st.uptime_s,
             bridge_setting_option_label(BRIDGE_SETTING_INVERTER, s.value[BRIDGE_SETTING_INVERTER]),
             leaf_shifter_name(st.shifter), st.torque_request_vcm / 4.0, st.torque_request_out / 4.0,
             st.torque_response / 2.0, st.charge_status, st.ac_charging ? "true" : "false", soc,
             (unsigned long)st.inverter.frames_modified, (unsigned long)st.inverter.frames_generated,
             (unsigned long)st.inverter.torque_clamped, (unsigned long)st.queue_overflow,
             (unsigned long)st.queue_high_water, vcm->running ? "true" : "false", (unsigned long)vcm->rx,
             (unsigned long)vcm->tx, (unsigned long)vcm->tx_dropped, (unsigned long)vcm->rx_overflow,
             (unsigned long)vcm->bus_off, inv->running ? "true" : "false", (unsigned long)inv->rx,
             (unsigned long)inv->tx, (unsigned long)inv->tx_dropped, (unsigned long)inv->rx_overflow,
             (unsigned long)inv->bus_off);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* ---- firmware update -------------------------------------------------------------------------- */
static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t update_page_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    return send_embedded(req, update_html_gz_start, update_html_gz_end, "text/html", "no-cache");
}

static esp_err_t update_upload_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (!bridge_is_parked()) {
        return send_text(req, "409 Conflict", "Shift to P (park) before updating the firmware");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return send_text(req, "500 Internal Server Error", "No OTA partition");
    }
    const size_t total = req->content_len;
    if (total == 0 || total > part->size) {
        return send_text(req, "400 Bad Request", "Image missing or larger than the OTA partition");
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return send_text(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    char *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        esp_ota_abort(ota);
        return send_text(req, "500 Internal Server Error", "Out of memory");
    }
    ESP_LOGW(TAG, "firmware update: %u bytes into %s", (unsigned)total, part->label);

    size_t received = 0;
    const char *fail = NULL;
    while (received < total) {
        const int r = httpd_req_recv(req, buf, (total - received) < OTA_CHUNK ? (total - received) : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            fail = "Upload interrupted";
            break;
        }
        if (!bridge_is_parked()) {
            fail = "Car left P during the update - aborted";
            break;
        }
        if (esp_ota_write(ota, buf, (size_t)r) != ESP_OK) {
            fail = "Not a valid ESP32 application image";
            break;
        }
        received += (size_t)r;
    }
    free(buf);

    if (fail != NULL) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "firmware update failed: %s", fail);
        return send_text(req, "400 Bad Request", fail);
    }
    err = esp_ota_end(ota); /* verifies the image checksum / SHA-256 */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware update rejected: %s", esp_err_to_name(err));
        return send_text(req, "400 Bad Request", "Image verification failed");
    }

    ESP_LOGW(TAG, "firmware update complete, rebooting");
    (void)send_text(req, "200 OK", "Update complete - the bridge restarts now");
    const esp_timer_create_args_t args = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1000 * 1000);
    }
    return ESP_OK;
}

/* ---- start ------------------------------------------------------------------------------------- */
esp_err_t web_server_start(void)
{
    init_auth();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.core_id = 0; /* keep CPU 1 for the bridge */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/index.html", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/static/*", .method = HTTP_GET, .handler = static_handler},
        {.uri = "/settings", .method = HTTP_POST, .handler = settings_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/ws",
         .method = HTTP_GET,
         .handler = ws_handler,
         .is_websocket = true,
         .ws_post_handshake_cb = ws_post_handshake},
        {.uri = "/update", .method = HTTP_GET, .handler = update_page_handler},
        {.uri = "/update", .method = HTTP_POST, .handler = update_upload_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
