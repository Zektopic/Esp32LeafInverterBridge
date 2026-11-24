/*
 * security.h - admin password, Wi-Fi credentials and web sessions.
 *
 * - The admin password protects every change made through the web UI / API
 *   (settings, firmware update, restart, factory reset, passwords). It is stored
 *   as a salted PBKDF2-HMAC-SHA256 hash in NVS (namespace "bridge").
 * - Until it has been changed, the factory default from menuconfig applies and
 *   security_is_default_password() is true; the web API then refuses every
 *   change except setting a new password.
 * - Logins create short-lived bearer tokens (random 128 bit, idle timeout).
 *   Repeated wrong passwords lock the login with an increasing delay.
 * - The access point SSID / password are stored in the same namespace.
 *
 * Not thread safe beyond the internal mutex: intended for the HTTP server task.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SECURITY_NVS_NAMESPACE      "bridge"
#define SECURITY_PASSWORD_MIN       8
#define SECURITY_PASSWORD_MAX       64
#define SECURITY_WIFI_SSID_MAX      32
#define SECURITY_WIFI_PASSWORD_MIN  8
#define SECURITY_WIFI_PASSWORD_MAX  63
#define SECURITY_TOKEN_LEN          32 /* hex characters */
#define SECURITY_SESSION_IDLE_S     (30 * 60)
#define SECURITY_SESSION_MAX_S      (12 * 60 * 60)

typedef enum {
    SECURITY_LOGIN_OK,
    SECURITY_LOGIN_BAD_PASSWORD,
    SECURITY_LOGIN_LOCKED,
} security_login_result_t;

esp_err_t security_init(void);

/* ---- admin password ------------------------------------------------------------ */
bool security_is_default_password(void);
/*
 * Check a password (e.g. from HTTP basic auth) with lockout accounting.
 * *retry_after_s is set when the result is SECURITY_LOGIN_LOCKED.
 */
security_login_result_t security_verify_password(const char *password, uint32_t *retry_after_s);
/* NULL if acceptable, otherwise a human readable reason. */
const char *security_check_new_password(const char *password);
esp_err_t security_set_admin_password(const char *password);

/* ---- sessions -------------------------------------------------------------------- */
/* Verify the password and open a session; token receives SECURITY_TOKEN_LEN hex chars + NUL. */
security_login_result_t security_login(const char *password, char token[SECURITY_TOKEN_LEN + 1],
                                       uint32_t *retry_after_s);
/* True if the token belongs to a live session (refreshes its idle timer). */
bool security_session_valid(const char *token);
void security_logout(const char *token);
void security_logout_all(void);

/* ---- Wi-Fi access point credentials ------------------------------------------------ */
void security_get_wifi(char *ssid, size_t ssid_len, char *password, size_t password_len);
bool security_is_default_wifi_password(void);
/* NULL if acceptable, otherwise a human readable reason. */
const char *security_check_wifi(const char *ssid, const char *password);
esp_err_t security_set_wifi(const char *ssid, const char *password);

/* Erase passwords and Wi-Fi credentials (factory reset). */
esp_err_t security_erase(void);

#ifdef __cplusplus
}
#endif
