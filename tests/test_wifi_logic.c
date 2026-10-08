#include "test_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ESP_OK                  0
#define ESP_ERR_INVALID_STATE   0x103

/* -------------------------------------------------------------------------
   Component Functions Under Test (Wi-Fi Manager FSM simulation)
   ------------------------------------------------------------------------- */

typedef struct {
    bool sta_enabled;
    bool sta_connected;
    char sta_ssid[33];
    char sta_password[65];
    char sta_ip[16];

    bool ap_enabled;
    char ap_ssid[33];
    char ap_password[65];
} sim_wifi_config_t;

static sim_wifi_config_t s_config;
static uint32_t s_sta_retry_count = 0;
static bool s_sta_pending_verification = false;
static char s_sta_prev_ssid[33] = {0};
static char s_sta_prev_pass[65] = {0};

static void sim_wifi_init(void) {
    memset(&s_config, 0, sizeof(s_config));
    s_config.sta_enabled = true;
    s_config.sta_connected = false;
    snprintf(s_config.sta_ssid, sizeof(s_config.sta_ssid), "MyHomeNetwork");
    snprintf(s_config.sta_password, sizeof(s_config.sta_password), "MySecretPass");
    s_config.ap_enabled = true;
    snprintf(s_config.ap_ssid, sizeof(s_config.ap_ssid), "VorotaBot-AP");

    s_sta_retry_count = 0;
    s_sta_pending_verification = false;
    memset(s_sta_prev_ssid, 0, sizeof(s_sta_prev_ssid));
    memset(s_sta_prev_pass, 0, sizeof(s_sta_prev_pass));
}

static void sim_wifi_enable_ap(void) {
    s_config.ap_enabled = true;
}

static int sim_wifi_disable_ap(void) {
    if (!s_config.ap_enabled) {
        return ESP_OK;
    }

    // Safety interlock: prevent turning off SoftAP if STA is not currently connected
    if (!s_config.sta_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    s_config.ap_enabled = false;
    return ESP_OK;
}

static void sim_wifi_on_sta_disconnected(void) {
    s_config.sta_connected = false;
    s_sta_retry_count++;

    if (s_config.sta_enabled) {
        // Rollback on 5 consecutive failures if pending verification
        if (s_sta_pending_verification && s_sta_retry_count >= 5) {
            if (strlen(s_sta_prev_ssid) > 0) {
                snprintf(s_config.sta_ssid, sizeof(s_config.sta_ssid), "%s", s_sta_prev_ssid);
                snprintf(s_config.sta_password, sizeof(s_config.sta_password), "%s", s_sta_prev_pass);
            } else {
                s_config.sta_enabled = false;
                s_config.sta_ssid[0] = '\0';
                s_config.sta_password[0] = '\0';
            }
            s_sta_pending_verification = false;
        }

        // Auto re-enable SoftAP fallback on 5 consecutive failures
        if (s_sta_retry_count >= 5 && !s_config.ap_enabled) {
            sim_wifi_enable_ap();
        }
    }
}

static void sim_wifi_on_sta_got_ip(const char *assigned_ip) {
    s_config.sta_connected = true;
    s_sta_retry_count = 0;
    s_sta_pending_verification = false;
    snprintf(s_config.sta_ip, sizeof(s_config.sta_ip), "%s", assigned_ip);
}

static void sim_wifi_set_new_credentials(const char *ssid, const char *pass) {
    snprintf(s_sta_prev_ssid, sizeof(s_sta_prev_ssid), "%s", s_config.sta_ssid);
    snprintf(s_sta_prev_pass, sizeof(s_sta_prev_pass), "%s", s_config.sta_password);

    snprintf(s_config.sta_ssid, sizeof(s_config.sta_ssid), "%s", ssid);
    snprintf(s_config.sta_password, sizeof(s_config.sta_password), "%s", pass);
    s_config.sta_enabled = true;
    s_config.sta_connected = false;
    s_sta_pending_verification = true;
    s_sta_retry_count = 0;
}

/* -------------------------------------------------------------------------
   Test Suites
   ------------------------------------------------------------------------- */

static void test_suite_ap_safety_interlock(void) {
    TEST_SUITE("Wi-Fi: Anti-Lockout SoftAP Safety Invariant");

    sim_wifi_init();
    s_config.ap_enabled = true;
    s_config.sta_connected = false; // Disconnected

    // Invariant: Turning off SoftAP when STA is disconnected MUST fail to prevent bricking
    int err = sim_wifi_disable_ap();
    ASSERT_EQ(err, ESP_ERR_INVALID_STATE);
    ASSERT_TRUE(s_config.ap_enabled);

    // When STA successfully connects, disabling SoftAP is allowed
    sim_wifi_on_sta_got_ip("192.168.1.100");
    ASSERT_TRUE(s_config.sta_connected);

    err = sim_wifi_disable_ap();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_FALSE(s_config.ap_enabled);

    // Redundant disable call returns ESP_OK (idempotent)
    err = sim_wifi_disable_ap();
    ASSERT_EQ(err, ESP_OK);
}

static void test_suite_retry_fallback(void) {
    TEST_SUITE("Wi-Fi: Connection Retry Threshold & SoftAP Fallback");

    sim_wifi_init();
    // Simulate initial scenario: STA was online, SoftAP was turned off by user
    s_config.sta_connected = true;
    s_config.ap_enabled = false;
    s_sta_retry_count = 0;

    // Disconnect 1: remains off
    sim_wifi_on_sta_disconnected();
    ASSERT_EQ(s_sta_retry_count, 1);
    ASSERT_FALSE(s_config.ap_enabled);

    // Disconnects 2..4: remains off
    sim_wifi_on_sta_disconnected();
    sim_wifi_on_sta_disconnected();
    sim_wifi_on_sta_disconnected();
    ASSERT_EQ(s_sta_retry_count, 4);
    ASSERT_FALSE(s_config.ap_enabled);

    // Disconnect 5: Threshold reached -> SoftAP MUST automatically enable
    sim_wifi_on_sta_disconnected();
    ASSERT_EQ(s_sta_retry_count, 5);
    ASSERT_TRUE(s_config.ap_enabled);

    // Reconnecting to Wi-Fi clears retry count
    sim_wifi_on_sta_got_ip("192.168.1.100");
    ASSERT_EQ(s_sta_retry_count, 0);
    ASSERT_TRUE(s_config.sta_connected);
}

static void test_suite_credential_rollback(void) {
    TEST_SUITE("Wi-Fi: Bad Credential Automatic Rollback");

    sim_wifi_init();
    snprintf(s_config.sta_ssid, sizeof(s_config.sta_ssid), "WorkingSSID");
    snprintf(s_config.sta_password, sizeof(s_config.sta_password), "WorkingPass");

    // User applies wrong password via Web UI
    sim_wifi_set_new_credentials("WorkingSSID", "TypoWrongPass");
    ASSERT_TRUE(s_sta_pending_verification);
    ASSERT_STR_EQ(s_config.sta_password, "TypoWrongPass");

    // Failures 1 through 4
    for (int i = 0; i < 4; i++) {
        sim_wifi_on_sta_disconnected();
        ASSERT_TRUE(s_sta_pending_verification);
    }

    // Failure 5: rollback triggered
    sim_wifi_on_sta_disconnected();
    ASSERT_FALSE(s_sta_pending_verification);
    ASSERT_STR_EQ(s_config.sta_password, "WorkingPass");
    ASSERT_STR_EQ(s_config.sta_ssid, "WorkingSSID");

    // Case 2: No previous network existed (brand new device)
    s_config.sta_ssid[0] = '\0';
    s_config.sta_password[0] = '\0';
    sim_wifi_set_new_credentials("NonExistentSSID", "Pass");

    for (int i = 0; i < 5; i++) {
        sim_wifi_on_sta_disconnected();
    }
    // When no previous SSID was stored, STA should be disabled
    ASSERT_FALSE(s_config.sta_enabled);
    ASSERT_FALSE(s_sta_pending_verification);
}

int main(void) {
    printf(ANSI_BOLD ">>> RUNNING SUITE: test_wifi_logic <<<" ANSI_RESET "\n");

    test_suite_ap_safety_interlock();
    test_suite_retry_fallback();
    test_suite_credential_rollback();

    PRINT_TEST_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
