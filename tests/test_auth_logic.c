#include "test_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "mbedtls/sha256.h"

/* -------------------------------------------------------------------------
   Component Functions Under Test (Isolated from ESP-IDF OS dependencies)
   ------------------------------------------------------------------------- */

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

    for (int i = 1; i < 5000; i++) {
        mbedtls_sha256(out_hash, 32, out_hash, 0);
    }
}

// Lockout simulation model
static uint32_t s_failed_attempts = 0;
static int64_t s_simulated_time_sec = 1000;
static int64_t s_last_failed_time_sec = 0;

static void sim_auth_record_failed_attempt(void) {
    s_failed_attempts++;
    s_last_failed_time_sec = s_simulated_time_sec;
}

static void sim_auth_reset_failed_attempts(void) {
    s_failed_attempts = 0;
}

static bool sim_auth_is_locked_out(uint32_t *out_wait_seconds) {
    if (s_failed_attempts < 5) return false;

    int64_t elapsed_sec = s_simulated_time_sec - s_last_failed_time_sec;
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

// Host header validation logic model
static bool sim_validate_host_header(const char *host_in,
                                     const char *sta_ip,
                                     const char *wg_ip,
                                     const char *fqdn) {
    if (!host_in) return false;
    char host_hdr[128];
    snprintf(host_hdr, sizeof(host_hdr), "%s", host_in);
    char *colon = strchr(host_hdr, ':');
    if (colon) *colon = '\0';

    bool host_ok = (strcasecmp(host_hdr, "192.168.4.1") == 0 ||
                    strcasecmp(host_hdr, "localhost") == 0 ||
                    strcasecmp(host_hdr, "127.0.0.1") == 0 ||
                    strcasecmp(host_hdr, "vorota.local") == 0 ||
                    strcasecmp(host_hdr, "device.local") == 0 ||
                    (sta_ip && strlen(sta_ip) > 0 && strcmp(host_hdr, sta_ip) == 0) ||
                    (wg_ip && strlen(wg_ip) > 0 && strncmp(host_hdr, wg_ip, strlen(host_hdr)) == 0) ||
                    (fqdn && strlen(fqdn) > 0 && strcasecmp(host_hdr, fqdn) == 0));
    return host_ok;
}

static bool sim_validate_csrf_headers(const char *http_method, const char *x_requested_with, const char *x_auth_token) {
    // Non-POST requests do not require custom CSRF headers
    if (strcmp(http_method, "POST") != 0) {
        return true;
    }
    bool has_custom_hdr = ((x_requested_with && strlen(x_requested_with) > 0) ||
                           (x_auth_token && strlen(x_auth_token) > 0));
    return has_custom_hdr;
}

/* -------------------------------------------------------------------------
   Test Suites
   ------------------------------------------------------------------------- */

static void test_suite_constant_time_memcmp(void) {
    TEST_SUITE("Auth: Constant-Time Memory Comparison");

    uint8_t buf1[32] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint8_t buf2[32] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint8_t buf3[32] = {0x00, 0xbb, 0xcc, 0xdd}; // Differ at byte 0
    uint8_t buf4[32] = {0xaa, 0xbb, 0xff, 0xdd}; // Differ at byte 2
    uint8_t buf5[32] = {0xaa, 0xbb, 0xcc, 0x00}; // Differ at byte 3
    uint8_t buf6[32] = {0xaa, 0xbb, 0xcc, 0xdd};
    buf6[31] = 0x01;                              // Differ at byte 31

    ASSERT_TRUE(constant_time_memcmp(buf1, buf2, 32));
    ASSERT_FALSE(constant_time_memcmp(buf1, buf3, 32));
    ASSERT_FALSE(constant_time_memcmp(buf1, buf4, 32));
    ASSERT_FALSE(constant_time_memcmp(buf1, buf5, 32));
    ASSERT_FALSE(constant_time_memcmp(buf1, buf6, 32));

    // Zero-length comparison always succeeds
    ASSERT_TRUE(constant_time_memcmp(buf1, buf3, 0));
}

static void test_suite_pbkdf_hash(void) {
    TEST_SUITE("Auth: 5000-Round Salted Key Stretching");

    uint8_t salt1[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t salt2[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0,  1,  2,  3,  4,  5,  6};

    uint8_t hash_a[32];
    uint8_t hash_b[32];
    uint8_t hash_c[32];
    uint8_t hash_d[32];

    compute_password_hash("SecretGatePass123", salt1, sizeof(salt1), hash_a);
    compute_password_hash("SecretGatePass123", salt1, sizeof(salt1), hash_b);
    compute_password_hash("SecretGatePass123", salt2, sizeof(salt2), hash_c);
    compute_password_hash("WrongPassword456",  salt1, sizeof(salt1), hash_d);

    // Deterministic: same password + salt produces identical hash
    ASSERT_TRUE(constant_time_memcmp(hash_a, hash_b, 32));

    // Salt uniqueness: different salt produces completely distinct hash
    ASSERT_FALSE(constant_time_memcmp(hash_a, hash_c, 32));

    // Password uniqueness: wrong password produces distinct hash
    ASSERT_FALSE(constant_time_memcmp(hash_a, hash_d, 32));

    // Ensure hash is not trivially all zeroes or unchanged
    uint8_t zero_hash[32] = {0};
    ASSERT_FALSE(constant_time_memcmp(hash_a, zero_hash, 32));
}

static void test_suite_lockout_exponential(void) {
    TEST_SUITE("Auth: Brute-Force Exponential Lockout & Backoff");

    sim_auth_reset_failed_attempts();
    s_simulated_time_sec = 1000;
    uint32_t wait_sec = 0;

    // Attempts 1 to 4: No lockout
    for (int i = 1; i <= 4; i++) {
        sim_auth_record_failed_attempt();
        ASSERT_FALSE(sim_auth_is_locked_out(&wait_sec));
    }
    ASSERT_EQ(s_failed_attempts, 4);

    // Attempt 5: 5 << 0 = 5s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 5);

    // Advance 4s -> still locked out (1s remaining)
    s_simulated_time_sec += 4;
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 1);

    // Advance 1s -> lockout expired (elapsed 5s >= 5s)
    s_simulated_time_sec += 1;
    ASSERT_FALSE(sim_auth_is_locked_out(&wait_sec));

    // Attempt 6: 5 << 1 = 10s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 10);

    // Attempt 7: 5 << 2 = 20s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 20);

    // Attempt 8: 5 << 3 = 40s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 40);

    // Attempt 9: 5 << 4 = 80s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 80);

    // Attempt 10: 5 << 5 = 160s lockout
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 160);

    // Attempt 11: 5 << 6 = 320s -> capped at 300s
    sim_auth_record_failed_attempt();
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 300);

    // Attempt 20: still capped at 300s
    s_failed_attempts = 20;
    s_last_failed_time_sec = s_simulated_time_sec;
    ASSERT_TRUE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(wait_sec, 300);

    // Reset failed attempts upon successful login
    sim_auth_reset_failed_attempts();
    ASSERT_FALSE(sim_auth_is_locked_out(&wait_sec));
    ASSERT_EQ(s_failed_attempts, 0);
}

static void test_suite_csrf_and_host_validation(void) {
    TEST_SUITE("Auth: CSRF & Host DNS-Rebinding Protection");

    // CSRF tests
    ASSERT_TRUE(sim_validate_csrf_headers("GET", NULL, NULL));
    ASSERT_FALSE(sim_validate_csrf_headers("POST", NULL, NULL));
    ASSERT_FALSE(sim_validate_csrf_headers("POST", "", ""));
    ASSERT_TRUE(sim_validate_csrf_headers("POST", "vorota", NULL));
    ASSERT_TRUE(sim_validate_csrf_headers("POST", NULL, "token_xyz123"));
    ASSERT_TRUE(sim_validate_csrf_headers("POST", "vorota", "token_xyz123"));

    // Host header tests
    const char *sta_ip = "192.168.1.150";
    const char *wg_ip  = "10.8.0.2";
    const char *fqdn   = "gate.example.com";

    // Valid hosts
    ASSERT_TRUE(sim_validate_host_header("192.168.4.1", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("192.168.4.1:80", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("localhost", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("localhost:8080", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("127.0.0.1", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("vorota.local", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("device.local", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("192.168.1.150", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("192.168.1.150:80", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("10.8.0.2", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("gate.example.com", sta_ip, wg_ip, fqdn));
    ASSERT_TRUE(sim_validate_host_header("GATE.EXAMPLE.COM", sta_ip, wg_ip, fqdn));

    // Malicious hosts (DNS rebinding attacks)
    ASSERT_FALSE(sim_validate_host_header("evil.com", sta_ip, wg_ip, fqdn));
    ASSERT_FALSE(sim_validate_host_header("attacker.org:80", sta_ip, wg_ip, fqdn));
    ASSERT_FALSE(sim_validate_host_header("gate.example.com.evil.com", sta_ip, wg_ip, fqdn));
    ASSERT_FALSE(sim_validate_host_header("192.168.1.151", sta_ip, wg_ip, fqdn));
    ASSERT_FALSE(sim_validate_host_header(NULL, sta_ip, wg_ip, fqdn));
}

int main(void) {
    printf(ANSI_BOLD ">>> RUNNING SUITE: test_auth_logic <<<" ANSI_RESET "\n");

    test_suite_constant_time_memcmp();
    test_suite_pbkdf_hash();
    test_suite_lockout_exponential();
    test_suite_csrf_and_host_validation();

    PRINT_TEST_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
