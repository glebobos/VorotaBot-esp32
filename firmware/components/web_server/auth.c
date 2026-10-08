#include "auth.h"
#include "http_util.h"
#include "wireguard_manager.h"
#include "wifi_manager.h"
#include "aws_route53.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "AUTH";

static bool s_auth_initialized = false;
static uint8_t s_salt[16] = {0};
static uint8_t s_hash[32] = {0};
static bool s_has_password = false;
static char s_session_token[40] = {0};

// Brute-force rate limiting
static uint32_t s_failed_attempts = 0;
static int64_t s_last_failed_time_us = 0;

static void hex_to_bin(const char *hex, uint8_t *bin, size_t bin_len) {
    for (size_t i = 0; i < bin_len; i++) {
        unsigned int val = 0;
        sscanf(&hex[i * 2], "%02x", &val);
        bin[i] = (uint8_t)val;
    }
}

static void bin_to_hex(const uint8_t *bin, size_t bin_len, char *hex) {
    for (size_t i = 0; i < bin_len; i++) {
        sprintf(&hex[i * 2], "%02x", bin[i]);
    }
    hex[bin_len * 2] = '\0';
}

static bool constant_time_memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *ua = (const uint8_t *)a;
    const uint8_t *ub = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (ua[i] ^ ub[i]);
    }
    return diff == 0;
}

static void compute_password_hash(const char *pass, const uint8_t *salt, size_t salt_len, uint8_t out_hash[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, salt_len);
    mbedtls_sha256_update(&ctx, (const unsigned char *)pass, strlen(pass));
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);

    // 5000 rounds of key stretching
    for (int i = 1; i < 5000; i++) {
        mbedtls_sha256(out_hash, 32, out_hash, 0);
    }
}

void auth_rotate_session_token(void) {
    uint8_t rand_bytes[16];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));
    bin_to_hex(rand_bytes, sizeof(rand_bytes), s_session_token);
}

void auth_init(void) {
    if (s_auth_initialized) return;

    auth_rotate_session_token();

    char salt_hex[36] = {0};
    char hash_hex[68] = {0};

    if (nvs_manager_get_str("web_salt", salt_hex, sizeof(salt_hex), "") == ESP_OK &&
        nvs_manager_get_str("web_hash", hash_hex, sizeof(hash_hex), "") == ESP_OK &&
        strlen(salt_hex) == 32 && strlen(hash_hex) == 64) {
        hex_to_bin(salt_hex, s_salt, sizeof(s_salt));
        hex_to_bin(hash_hex, s_hash, sizeof(s_hash));
        s_has_password = true;
        ESP_LOGI(TAG, "Loaded hashed web password from NVS");
    } else {
        // Check for legacy plaintext migration
        char legacy_pass[64] = {0};
        if (nvs_manager_get_str("web_pass", legacy_pass, sizeof(legacy_pass), "") == ESP_OK &&
            strlen(legacy_pass) > 0) {
            ESP_LOGI(TAG, "Migrating legacy plaintext web password to salted PBKDF hash");
            auth_set_password(legacy_pass);
            nvs_manager_erase_key("web_pass");
        } else {
            s_has_password = false;
            ESP_LOGI(TAG, "No web password configured (Setup mode active)");
        }
    }

    s_auth_initialized = true;
}

bool auth_is_password_set(void) {
    auth_init();
    return s_has_password;
}

const char* auth_get_session_token(void) {
    auth_init();
    return s_session_token;
}

bool auth_is_trusted_tunnel(httpd_req_t *req) {
    if (!req) return false;
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return false;

    wireguard_status_t wg_st;
    wireguard_manager_get_status(&wg_st);
    if (!wg_st.is_connected || strlen(wg_st.assigned_ip) == 0) {
        return false;
    }

    struct sockaddr_in local_addr = {0};
    socklen_t addr_len = sizeof(local_addr);
    struct sockaddr_in peer_addr = {0};
    socklen_t peer_len = sizeof(peer_addr);

    if (getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len) != 0 ||
        getpeername(sockfd, (struct sockaddr *)&peer_addr, &peer_len) != 0) {
        return false;
    }

    char clean_wg_ip[32];
    snprintf(clean_wg_ip, sizeof(clean_wg_ip), "%s", wg_st.assigned_ip);
    char *slash = strchr(clean_wg_ip, '/');
    if (slash) *slash = '\0';

    ip4_addr_t wg_ip, local_ip, peer_ip, wg_mask;
    if (ip4addr_aton(clean_wg_ip, &wg_ip) != 1) return false;
    local_ip.addr = local_addr.sin_addr.s_addr;
    peer_ip.addr = peer_addr.sin_addr.s_addr;

    // Must be addressed to WireGuard interface IP
    if (local_ip.addr != wg_ip.addr) {
        return false;
    }

    // Default /24 subnet for WireGuard tunnel
    ip4addr_aton("255.255.255.0", &wg_mask);

    // Peer must be in WireGuard tunnel subnet and not self
    if ((peer_ip.addr & wg_mask.addr) != (wg_ip.addr & wg_mask.addr)) {
        return false;
    }
    if (peer_ip.addr == wg_ip.addr) {
        return false;
    }

    // Check for collision with Wi-Fi STA or SoftAP subnets
    char peer_ip_str[32] = {0};
    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip_str, sizeof(peer_ip_str));

    if (strncmp(peer_ip_str, "192.168.4.", 10) == 0) {
        return false;
    }
    if (wifi_manager_is_in_sta_subnet(peer_ip_str)) {
        return false;
    }

    ESP_LOGD(TAG, "Verified WireGuard peer: %s", peer_ip_str);
    return true;
}

bool auth_validate_csrf(httpd_req_t *req) {
    if (!req) return false;

    // Only validate mutating methods (POST)
    if (req->method != HTTP_POST) {
        return true;
    }

    // 1. Require custom header X-Requested-With or valid X-Auth-Token (browsers cannot send cross-origin without preflight)
    char req_with[32] = {0};
    char auth_token[64] = {0};
    bool has_custom_hdr = (httpd_req_get_hdr_value_str(req, "X-Requested-With", req_with, sizeof(req_with)) == ESP_OK ||
                           httpd_req_get_hdr_value_str(req, "X-Auth-Token", auth_token, sizeof(auth_token)) == ESP_OK);

    if (!has_custom_hdr) {
        ESP_LOGW(TAG, "CSRF reject: Missing custom header (X-Requested-With or X-Auth-Token)");
        http_send_error(req, "403 Forbidden", "Cross-site request blocked: custom header required");
        return false;
    }

    // 2. Host header verification against DNS rebinding
    char host_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, sizeof(host_hdr)) == ESP_OK) {
        char *colon = strchr(host_hdr, ':');
        if (colon) *colon = '\0';

        wifi_mgr_config_t w_cfg;
        wifi_manager_get_config(&w_cfg);

        wireguard_status_t wg_st;
        wireguard_manager_get_status(&wg_st);

        aws_route53_status_t r53_st;
        aws_route53_get_status(&r53_st);

        bool host_ok = (strcasecmp(host_hdr, "192.168.4.1") == 0 ||
                        strcasecmp(host_hdr, "localhost") == 0 ||
                        strcasecmp(host_hdr, "127.0.0.1") == 0 ||
                        strcasecmp(host_hdr, "vorota.local") == 0 ||
                        strcasecmp(host_hdr, "device.local") == 0 ||
                        (strlen(w_cfg.sta_ip) > 0 && strcmp(host_hdr, w_cfg.sta_ip) == 0) ||
                        (strlen(wg_st.assigned_ip) > 0 && strncmp(host_hdr, wg_st.assigned_ip, strlen(host_hdr)) == 0) ||
                        (strlen(r53_st.fqdn) > 0 && strcasecmp(host_hdr, r53_st.fqdn) == 0));

        if (!host_ok) {
            ESP_LOGW(TAG, "DNS rebinding reject: Unrecognized Host header '%s'", host_hdr);
            http_send_error(req, "403 Forbidden", "Invalid Host header");
            return false;
        }
    }

    return true;
}

bool auth_is_authenticated(httpd_req_t *req) {
    auth_init();

    // WireGuard connection is cryptographically authenticated
    if (auth_is_trusted_tunnel(req)) {
        return true;
    }

    // If no password is configured, non-WireGuard access requires setup mode
    if (!s_has_password) {
        return false;
    }

    // Check X-Auth-Token header
    char token_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token_hdr, sizeof(token_hdr)) == ESP_OK) {
        if (strlen(s_session_token) > 0 && constant_time_memcmp(token_hdr, s_session_token, 32)) {
            return true;
        }
    }

    return false;
}

bool auth_check_or_reject(httpd_req_t *req) {
    if (!auth_validate_csrf(req)) {
        return false;
    }

    if (auth_is_authenticated(req)) {
        return true;
    }

    if (!auth_is_password_set()) {
        http_send_error(req, "403 Forbidden", "Initial setup required: please configure administrator password");
        return false;
    }

    http_send_error(req, "401 Unauthorized", "Unauthorized");
    return false;
}

bool auth_verify_password(const char *password) {
    auth_init();
    if (!s_has_password || !password) return false;

    uint8_t test_hash[32];
    compute_password_hash(password, s_salt, sizeof(s_salt), test_hash);
    return constant_time_memcmp(test_hash, s_hash, sizeof(s_hash));
}

bool auth_set_password(const char *new_password) {
    auth_init();

    if (!new_password || strlen(new_password) == 0) {
        s_has_password = false;
        nvs_manager_erase_key("web_salt");
        nvs_manager_erase_key("web_hash");
        memset(s_salt, 0, sizeof(s_salt));
        memset(s_hash, 0, sizeof(s_hash));
        auth_rotate_session_token();
        ESP_LOGI(TAG, "Web password erased (reverted to Setup mode)");
        return true;
    }

    esp_fill_random(s_salt, sizeof(s_salt));
    compute_password_hash(new_password, s_salt, sizeof(s_salt), s_hash);

    char salt_hex[36] = {0};
    char hash_hex[68] = {0};
    bin_to_hex(s_salt, sizeof(s_salt), salt_hex);
    bin_to_hex(s_hash, sizeof(s_hash), hash_hex);

    nvs_manager_set_str("web_salt", salt_hex);
    nvs_manager_set_str("web_hash", hash_hex);
    s_has_password = true;
    auth_rotate_session_token();
    ESP_LOGI(TAG, "New salted PBKDF web password stored in NVS");
    return true;
}

bool auth_is_locked_out(uint32_t *out_wait_seconds) {
    if (s_failed_attempts < 5) return false;

    int64_t now = esp_timer_get_time();
    int64_t elapsed_sec = (now - s_last_failed_time_us) / 1000000;

    // Exponential lockout: 5 fails = 5s, 6 = 10s, 7 = 20s, up to 300s
    uint32_t lock_duration = 5 << (s_failed_attempts - 5);
    if (lock_duration > 300) lock_duration = 300;

    if (elapsed_sec < lock_duration) {
        if (out_wait_seconds) {
            *out_wait_seconds = (uint32_t)(lock_duration - elapsed_sec);
        }
        return true;
    }

    return false;
}

void auth_record_failed_attempt(void) {
    s_failed_attempts++;
    s_last_failed_time_us = esp_timer_get_time();
    ESP_LOGW(TAG, "Failed login attempt recorded (total: %lu)", (unsigned long)s_failed_attempts);
}

void auth_reset_failed_attempts(void) {
    s_failed_attempts = 0;
}
