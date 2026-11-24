/*
 * security.c - admin password, Wi-Fi credentials and web sessions.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "security.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "security";

#define KDF_ITERATIONS       4096u
#define SALT_LEN             16
#define HASH_LEN             32
#define MAX_SESSIONS         4
#define LOCK_AFTER_FAILURES  5u
#define LOCK_BASE_S          30u
#define LOCK_MAX_S           300u
#define MAX_PASSWORD_INPUT   128

#define KEY_SALT "adm_salt"
#define KEY_HASH "adm_hash"
#define KEY_ITER "adm_iter"
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"

_Static_assert(sizeof(CONFIG_BRIDGE_WIFI_PASSWORD) - 1 >= SECURITY_WIFI_PASSWORD_MIN,
               "Wi-Fi password must have at least 8 characters");
_Static_assert(sizeof(CONFIG_BRIDGE_WIFI_PASSWORD) - 1 <= SECURITY_WIFI_PASSWORD_MAX,
               "Wi-Fi password must have at most 63 characters");
_Static_assert(sizeof(CONFIG_BRIDGE_WIFI_SSID) - 1 >= 1 && sizeof(CONFIG_BRIDGE_WIFI_SSID) - 1 <= SECURITY_WIFI_SSID_MAX,
               "SSID must have 1 to 32 characters");

/* The factory admin password: menuconfig value, or the Wi-Fi password if empty. */
#define DEFAULT_ADMIN_PASSWORD \
    ((sizeof(CONFIG_BRIDGE_ADMIN_PASSWORD) > 1) ? CONFIG_BRIDGE_ADMIN_PASSWORD : CONFIG_BRIDGE_WIFI_PASSWORD)

typedef struct {
    bool used;
    char token[SECURITY_TOKEN_LEN + 1];
    int64_t created_us;
    int64_t last_us;
} session_t;

static SemaphoreHandle_t s_mutex;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static uint32_t s_iterations = KDF_ITERATIONS;
static bool s_default_password = true;
static char s_ssid[SECURITY_WIFI_SSID_MAX + 1];
static char s_wifi_password[SECURITY_WIFI_PASSWORD_MAX + 1];
static session_t s_sessions[MAX_SESSIONS];
static uint32_t s_failures;
static int64_t s_locked_until_us;

/* ---- PBKDF2-HMAC-SHA256 (one 32 byte block) on top of PSA hash ----------------------- */
static psa_status_t hmac_from_bases(const psa_hash_operation_t *inner_base, const psa_hash_operation_t *outer_base,
                                    const uint8_t *m1, size_t l1, const uint8_t *m2, size_t l2,
                                    uint8_t out[HASH_LEN])
{
    uint8_t inner[HASH_LEN];
    size_t n = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t st = psa_hash_clone(inner_base, &op);
    if (st == PSA_SUCCESS) {
        st = psa_hash_update(&op, m1, l1);
    }
    if (st == PSA_SUCCESS && m2 != NULL) {
        st = psa_hash_update(&op, m2, l2);
    }
    if (st == PSA_SUCCESS) {
        st = psa_hash_finish(&op, inner, sizeof(inner), &n);
    }
    psa_hash_abort(&op);
    if (st != PSA_SUCCESS) {
        return st;
    }
    op = psa_hash_operation_init();
    st = psa_hash_clone(outer_base, &op);
    if (st == PSA_SUCCESS) {
        st = psa_hash_update(&op, inner, sizeof(inner));
    }
    if (st == PSA_SUCCESS) {
        st = psa_hash_finish(&op, out, HASH_LEN, &n);
    }
    psa_hash_abort(&op);
    return st;
}

static esp_err_t pbkdf2_sha256(const char *password, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                               uint8_t out[HASH_LEN])
{
    uint8_t key[64] = {0};
    uint8_t pad[64];
    const size_t pw_len = strlen(password);
    size_t n = 0;
    psa_status_t st = PSA_SUCCESS;

    if (pw_len > sizeof(key)) {
        st = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)password, pw_len, key, HASH_LEN, &n);
    } else {
        memcpy(key, password, pw_len);
    }

    psa_hash_operation_t inner_base = PSA_HASH_OPERATION_INIT;
    psa_hash_operation_t outer_base = PSA_HASH_OPERATION_INIT;
    if (st == PSA_SUCCESS) {
        for (size_t i = 0; i < sizeof(pad); i++) {
            pad[i] = (uint8_t)(key[i] ^ 0x36);
        }
        st = psa_hash_setup(&inner_base, PSA_ALG_SHA_256);
        if (st == PSA_SUCCESS) {
            st = psa_hash_update(&inner_base, pad, sizeof(pad));
        }
    }
    if (st == PSA_SUCCESS) {
        for (size_t i = 0; i < sizeof(pad); i++) {
            pad[i] = (uint8_t)(key[i] ^ 0x5C);
        }
        st = psa_hash_setup(&outer_base, PSA_ALG_SHA_256);
        if (st == PSA_SUCCESS) {
            st = psa_hash_update(&outer_base, pad, sizeof(pad));
        }
    }

    uint8_t u[HASH_LEN];
    if (st == PSA_SUCCESS) {
        static const uint8_t block_index[4] = {0, 0, 0, 1};
        st = hmac_from_bases(&inner_base, &outer_base, salt, salt_len, block_index, sizeof(block_index), u);
        memcpy(out, u, HASH_LEN);
    }
    for (uint32_t i = 1; i < iterations && st == PSA_SUCCESS; i++) {
        st = hmac_from_bases(&inner_base, &outer_base, u, sizeof(u), NULL, 0, u);
        for (int j = 0; j < HASH_LEN; j++) {
            out[j] ^= u[j];
        }
    }
    psa_hash_abort(&inner_base);
    psa_hash_abort(&outer_base);
    memset(key, 0, sizeof(key));
    memset(pad, 0, sizeof(pad));
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 failed (PSA status %d)", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/* ---- init ------------------------------------------------------------------------------- */
esp_err_t security_init(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_ssid, CONFIG_BRIDGE_WIFI_SSID, sizeof(s_ssid));
    strlcpy(s_wifi_password, CONFIG_BRIDGE_WIFI_PASSWORD, sizeof(s_wifi_password));
    s_default_password = true;

    nvs_handle_t h;
    if (nvs_open(SECURITY_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t salt_len = sizeof(s_salt), hash_len = sizeof(s_hash);
        uint32_t iterations = 0;
        if (nvs_get_blob(h, KEY_SALT, s_salt, &salt_len) == ESP_OK && salt_len == sizeof(s_salt) &&
            nvs_get_blob(h, KEY_HASH, s_hash, &hash_len) == ESP_OK && hash_len == sizeof(s_hash) &&
            nvs_get_u32(h, KEY_ITER, &iterations) == ESP_OK && iterations >= 1000 && iterations <= 100000) {
            s_iterations = iterations;
            s_default_password = false;
        }
        char ssid[sizeof(s_ssid)], pass[sizeof(s_wifi_password)];
        size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
        if (nvs_get_str(h, KEY_SSID, ssid, &ssid_len) == ESP_OK && nvs_get_str(h, KEY_PASS, pass, &pass_len) == ESP_OK &&
            security_check_wifi(ssid, pass) == NULL) {
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            strlcpy(s_wifi_password, pass, sizeof(s_wifi_password));
        }
        nvs_close(h);
    }

    if (s_default_password) {
        /* No password set yet: hash the factory default with a fresh salt. */
        esp_fill_random(s_salt, sizeof(s_salt));
        s_iterations = KDF_ITERATIONS;
        esp_err_t err = pbkdf2_sha256(DEFAULT_ADMIN_PASSWORD, s_salt, sizeof(s_salt), s_iterations, s_hash);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "admin password is the factory default - changes are blocked until it is changed");
    }
    ESP_LOGI(TAG, "security ready (access point \"%s\"%s)", s_ssid,
             security_is_default_wifi_password() ? ", default Wi-Fi password" : "");
    return ESP_OK;
}

/* ---- admin password --------------------------------------------------------------------------- */
bool security_is_default_password(void)
{
    return s_default_password;
}

static uint32_t lock_remaining_s(int64_t now)
{
    return (now < s_locked_until_us) ? (uint32_t)((s_locked_until_us - now + 999999) / 1000000) : 0u;
}

security_login_result_t security_verify_password(const char *password, uint32_t *retry_after_s)
{
    uint8_t hash[HASH_LEN];
    security_login_result_t result;
    uint32_t retry = 0;

    lock();
    const int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us) {
        retry = lock_remaining_s(now);
        result = SECURITY_LOGIN_LOCKED;
    } else if (password != NULL && strnlen(password, MAX_PASSWORD_INPUT + 1) <= MAX_PASSWORD_INPUT &&
               pbkdf2_sha256(password, s_salt, sizeof(s_salt), s_iterations, hash) == ESP_OK &&
               equal_ct(hash, s_hash, sizeof(hash))) {
        s_failures = 0;
        result = SECURITY_LOGIN_OK;
    } else {
        s_failures++;
        if (s_failures >= LOCK_AFTER_FAILURES) {
            const uint32_t shift = s_failures - LOCK_AFTER_FAILURES;
            const uint32_t secs = (shift >= 4) ? LOCK_MAX_S : (LOCK_BASE_S << shift);
            s_locked_until_us = esp_timer_get_time() + (int64_t)((secs > LOCK_MAX_S) ? LOCK_MAX_S : secs) * 1000000;
            retry = lock_remaining_s(esp_timer_get_time());
            ESP_LOGW(TAG, "%lu wrong passwords, login locked for %lu s", (unsigned long)s_failures,
                     (unsigned long)retry);
        }
        result = SECURITY_LOGIN_BAD_PASSWORD;
    }
    unlock();
    memset(hash, 0, sizeof(hash));
    if (retry_after_s != NULL) {
        *retry_after_s = retry;
    }
    return result;
}

static bool printable_ascii(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x20 || *p > 0x7E) {
            return false;
        }
    }
    return true;
}

const char *security_check_new_password(const char *password)
{
    if (password == NULL) {
        return "Password missing";
    }
    const size_t len = strlen(password);
    if (len < SECURITY_PASSWORD_MIN) {
        return "Use at least 8 characters";
    }
    if (len > SECURITY_PASSWORD_MAX) {
        return "Use at most 64 characters";
    }
    for (const unsigned char *p = (const unsigned char *)password; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return "Control characters are not allowed";
        }
    }
    if (strcmp(password, DEFAULT_ADMIN_PASSWORD) == 0 || strcmp(password, CONFIG_BRIDGE_WIFI_PASSWORD) == 0) {
        return "Choose a password different from the factory default";
    }
    return NULL;
}

esp_err_t security_set_admin_password(const char *password)
{
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    if (security_check_new_password(password) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = pbkdf2_sha256(password, salt, sizeof(salt), KDF_ITERATIONS, hash);
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_SALT, salt, sizeof(salt));
        if (err == ESP_OK) {
            err = nvs_set_blob(h, KEY_HASH, hash, sizeof(hash));
        }
        if (err == ESP_OK) {
            err = nvs_set_u32(h, KEY_ITER, KDF_ITERATIONS);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the admin password failed: %s", esp_err_to_name(err));
        return err;
    }
    lock();
    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_hash, hash, sizeof(hash));
    s_iterations = KDF_ITERATIONS;
    s_default_password = false;
    unlock();
    ESP_LOGI(TAG, "admin password changed");
    return ESP_OK;
}

/* ---- sessions --------------------------------------------------------------------------------- */
static bool session_expired(const session_t *s, int64_t now)
{
    return (now - s->last_us) > (int64_t)SECURITY_SESSION_IDLE_S * 1000000 ||
           (now - s->created_us) > (int64_t)SECURITY_SESSION_MAX_S * 1000000;
}

security_login_result_t security_login(const char *password, char token[SECURITY_TOKEN_LEN + 1],
                                       uint32_t *retry_after_s)
{
    const security_login_result_t r = security_verify_password(password, retry_after_s);
    if (r != SECURITY_LOGIN_OK) {
        return r;
    }
    uint8_t raw[SECURITY_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(&token[i * 2], 3, "%02x", raw[i]);
    }

    lock();
    const int64_t now = esp_timer_get_time();
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].used || session_expired(&s_sessions[i], now)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { /* all in use: replace the least recently used */
        slot = 0;
        for (int i = 1; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].last_us < s_sessions[slot].last_us) {
                slot = i;
            }
        }
    }
    s_sessions[slot].used = true;
    s_sessions[slot].created_us = now;
    s_sessions[slot].last_us = now;
    memcpy(s_sessions[slot].token, token, SECURITY_TOKEN_LEN + 1);
    unlock();
    return SECURITY_LOGIN_OK;
}

static int find_session(const char *token, int64_t now)
{
    if (token == NULL || strnlen(token, SECURITY_TOKEN_LEN + 1) != SECURITY_TOKEN_LEN) {
        return -1;
    }
    int found = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].used &&
            equal_ct((const uint8_t *)s_sessions[i].token, (const uint8_t *)token, SECURITY_TOKEN_LEN)) {
            if (session_expired(&s_sessions[i], now)) {
                memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            } else {
                found = i;
            }
        }
    }
    return found;
}

bool security_session_valid(const char *token)
{
    lock();
    const int64_t now = esp_timer_get_time();
    const int i = find_session(token, now);
    if (i >= 0) {
        s_sessions[i].last_us = now;
    }
    unlock();
    return i >= 0;
}

void security_logout(const char *token)
{
    lock();
    const int i = find_session(token, esp_timer_get_time());
    if (i >= 0) {
        memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
    }
    unlock();
}

void security_logout_all(void)
{
    lock();
    memset(s_sessions, 0, sizeof(s_sessions));
    unlock();
}

/* ---- Wi-Fi ------------------------------------------------------------------------------------------ */
void security_get_wifi(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    lock();
    if (ssid != NULL) {
        strlcpy(ssid, s_ssid, ssid_len);
    }
    if (password != NULL) {
        strlcpy(password, s_wifi_password, password_len);
    }
    unlock();
}

bool security_is_default_wifi_password(void)
{
    return strcmp(s_wifi_password, CONFIG_BRIDGE_WIFI_PASSWORD) == 0;
}

const char *security_check_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return "Network name and password are required";
    }
    const size_t ssid_len = strlen(ssid), pass_len = strlen(password);
    if (ssid_len < 1 || ssid_len > SECURITY_WIFI_SSID_MAX) {
        return "The network name needs 1 to 32 characters";
    }
    if (pass_len < SECURITY_WIFI_PASSWORD_MIN || pass_len > SECURITY_WIFI_PASSWORD_MAX) {
        return "The Wi-Fi password needs 8 to 63 characters";
    }
    if (!printable_ascii(ssid) || !printable_ascii(password)) {
        return "Use plain ASCII letters, digits and symbols only";
    }
    return NULL;
}

esp_err_t security_set_wifi(const char *ssid, const char *password)
{
    if (security_check_wifi(ssid, password) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, KEY_PASS, password);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the Wi-Fi settings failed: %s", esp_err_to_name(err));
        return err;
    }
    lock();
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_wifi_password, password, sizeof(s_wifi_password));
    unlock();
    ESP_LOGI(TAG, "access point settings changed (\"%s\")", ssid);
    return ESP_OK;
}

esp_err_t security_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SECURITY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    security_logout_all();
    return err;
}
