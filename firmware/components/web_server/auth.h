#pragma once

#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize authentication state and load salt/hash from NVS.
 */
void auth_init(void);

/**
 * @brief Check if request originates from a cryptographically verified WireGuard peer.
 */
bool auth_is_trusted_tunnel(httpd_req_t *req);

/**
 * @brief Check if a web access password has been configured.
 */
bool auth_is_password_set(void);

/**
 * @brief Check if caller is authenticated (either via WireGuard tunnel or valid session token).
 */
bool auth_is_authenticated(httpd_req_t *req);

/**
 * @brief If unauthenticated, sends 401 Unauthorized or 403 Setup Required JSON and returns false.
 */
bool auth_check_or_reject(httpd_req_t *req);

/**
 * @brief Anti-CSRF and DNS-rebinding validation for mutating (POST) endpoints.
 * Requires X-Requested-With or X-Auth-Token, and validates Host header.
 */
bool auth_validate_csrf(httpd_req_t *req);

/**
 * @brief Verify input password against stored salt and PBKDF-style hash in constant time.
 */
bool auth_verify_password(const char *password);

/**
 * @brief Update or set new web password (hashes with fresh random salt and saves to NVS).
 * If new_password is empty, clears password protection.
 */
bool auth_set_password(const char *new_password);

/**
 * @brief Get active session token.
 */
const char* auth_get_session_token(void);

/**
 * @brief Rotate session token upon login or password change.
 */
void auth_rotate_session_token(void);

/**
 * @brief Check if login attempts are currently rate-limited (lockout).
 */
bool auth_is_locked_out(uint32_t *out_wait_seconds);

/**
 * @brief Record a failed login attempt to trigger lockout delays.
 */
void auth_record_failed_attempt(void);

/**
 * @brief Reset failed attempt counter upon successful login.
 */
void auth_reset_failed_attempts(void);

#ifdef __cplusplus
}
#endif
