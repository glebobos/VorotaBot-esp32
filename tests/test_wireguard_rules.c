#include "test_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
   Component Functions Under Test
   ------------------------------------------------------------------------- */

static void trim_whitespace(char *str) {
    if (!str) return;
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    if (start > str) {
        memmove(str, start, end - start + 1);
    }
}

static bool is_valid_wg_key(const char *key) {
    if (!key || strlen(key) != 44) return false;
    for (int i = 0; i < 43; i++) {
        char c = key[i];
        if (!isalnum((unsigned char)c) && c != '+' && c != '/') return false;
    }
    return key[43] == '=';
}

typedef struct {
    char private_key[64];
    char address[32];
    char peer_public_key[64];
    char preshared_key[64];
    char peer_endpoint[64];
    uint16_t peer_port;
    char allowed_ips[64];
    uint16_t persistent_keepalive;
    bool enabled;
} sim_wg_config_t;

static int sim_parse_wg_conf(const char *conf_text, sim_wg_config_t *out_cfg) {
    if (!conf_text || !out_cfg || strlen(conf_text) == 0) return -1;

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->peer_port = 443;
    out_cfg->persistent_keepalive = 25;
    out_cfg->enabled = true;
    snprintf(out_cfg->address, sizeof(out_cfg->address), "10.0.0.2");
    snprintf(out_cfg->allowed_ips, sizeof(out_cfg->allowed_ips), "0.0.0.0/0");

    char *copy = strdup(conf_text);
    if (!copy) return -2;

    char *line = strtok(copy, "\r\n");
    while (line) {
        trim_whitespace(line);
        if (line[0] == '#' || line[0] == '[' || strlen(line) == 0) {
            line = strtok(NULL, "\r\n");
            continue;
        }

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            trim_whitespace(key);
            trim_whitespace(val);

            if (strcasecmp(key, "PrivateKey") == 0) {
                snprintf(out_cfg->private_key, sizeof(out_cfg->private_key), "%s", val);
            } else if (strcasecmp(key, "Address") == 0) {
                snprintf(out_cfg->address, sizeof(out_cfg->address), "%s", val);
            } else if (strcasecmp(key, "PublicKey") == 0) {
                snprintf(out_cfg->peer_public_key, sizeof(out_cfg->peer_public_key), "%s", val);
            } else if (strcasecmp(key, "PresharedKey") == 0) {
                snprintf(out_cfg->preshared_key, sizeof(out_cfg->preshared_key), "%s", val);
            } else if (strcasecmp(key, "Endpoint") == 0) {
                char *colon = strrchr(val, ':');
                if (colon) {
                    *colon = '\0';
                    snprintf(out_cfg->peer_endpoint, sizeof(out_cfg->peer_endpoint), "%s", val);
                    out_cfg->peer_port = (uint16_t)atoi(colon + 1);
                } else {
                    snprintf(out_cfg->peer_endpoint, sizeof(out_cfg->peer_endpoint), "%s", val);
                }
            } else if (strcasecmp(key, "AllowedIPs") == 0) {
                snprintf(out_cfg->allowed_ips, sizeof(out_cfg->allowed_ips), "%s", val);
            } else if (strcasecmp(key, "PersistentKeepalive") == 0) {
                out_cfg->persistent_keepalive = (uint16_t)atoi(val);
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(copy);

    // Validate parsed fields
    if (!is_valid_wg_key(out_cfg->private_key)) return -3;
    if (!is_valid_wg_key(out_cfg->peer_public_key)) return -4;
    if (strlen(out_cfg->preshared_key) > 0 && !is_valid_wg_key(out_cfg->preshared_key)) return -5;
    if (strlen(out_cfg->peer_endpoint) == 0 || out_cfg->peer_port == 0) return -6;
    if (strlen(out_cfg->address) == 0) return -7;

    return 0; // Success
}

// WireGuard Trust Evaluation Rule
static bool sim_eval_trusted_wg_tunnel(bool wg_connected,
                                       const char *assigned_wg_ip,
                                       const char *local_dest_ip_str,
                                       const char *peer_remote_ip_str,
                                       const char *sta_subnet_prefix) {
    if (!wg_connected || !assigned_wg_ip || strlen(assigned_wg_ip) == 0) {
        return false;
    }
    if (!local_dest_ip_str || !peer_remote_ip_str) {
        return false;
    }

    char clean_wg_ip[32];
    snprintf(clean_wg_ip, sizeof(clean_wg_ip), "%s", assigned_wg_ip);
    char *slash = strchr(clean_wg_ip, '/');
    if (slash) *slash = '\0';

    struct in_addr wg_ip, local_ip, peer_ip, wg_mask;
    if (inet_aton(clean_wg_ip, &wg_ip) == 0) return false;
    if (inet_aton(local_dest_ip_str, &local_ip) == 0) return false;
    if (inet_aton(peer_remote_ip_str, &peer_ip) == 0) return false;

    // Destination must match WireGuard interface IP
    if (local_ip.s_addr != wg_ip.s_addr) {
        return false;
    }

    // Default /24 subnet for WireGuard
    inet_aton("255.255.255.0", &wg_mask);

    // Peer must be in WireGuard tunnel subnet
    if ((peer_ip.s_addr & wg_mask.s_addr) != (wg_ip.s_addr & wg_mask.s_addr)) {
        return false;
    }

    // Peer cannot be self
    if (peer_ip.s_addr == wg_ip.s_addr) {
        return false;
    }

    // Reject SoftAP subnet collision (192.168.4.x)
    if (strncmp(peer_remote_ip_str, "192.168.4.", 10) == 0) {
        return false;
    }

    // Reject Wi-Fi STA subnet collision if applicable
    if (sta_subnet_prefix && strlen(sta_subnet_prefix) > 0) {
        if (strncmp(peer_remote_ip_str, sta_subnet_prefix, strlen(sta_subnet_prefix)) == 0) {
            return false;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------
   Test Suites
   ------------------------------------------------------------------------- */

static void test_suite_key_validation(void) {
    TEST_SUITE("WireGuard: Base64 Key Validation");

    // Standard valid 44-char base64 keys
    const char *k1 = "YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWE=";
    const char *k2 = "c08vL3B1YmxpY2tleStleGFtcGxlK2Zvcit0ZXN0aW4=";
    const char *k3 = "47GZTm5GIchThnqdKwPt9Wpv4iaEDOzFsAs3lxVeug0=";
    ASSERT_TRUE(is_valid_wg_key(k1));
    ASSERT_TRUE(is_valid_wg_key(k2));
    ASSERT_TRUE(is_valid_wg_key(k3));

    // Invalid length
    ASSERT_FALSE(is_valid_wg_key(""));
    ASSERT_FALSE(is_valid_wg_key("short_key="));
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWE==")); // 45 chars
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYW"));   // 42 chars

    // Missing trailing '='
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWEA")); // 44 chars, ends in 'A'
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWE1")); // 44 chars, ends in '1'

    // Invalid characters (not base64)
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFh YWFhYWFhYWFhYWFhYWFhYWE=")); // space
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFh-YWFhYWFhYWFhYWFhYWFhYWE=")); // '-'
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFh_YWFhYWFhYWFhYWFhYWFhYWE=")); // '_'
    ASSERT_FALSE(is_valid_wg_key("YWFhYWFhYWFhYWFhYWFh$YWFhYWFhYWFhYWFhYWFhYWE=")); // '$'
    ASSERT_FALSE(is_valid_wg_key(NULL));
}

static void test_suite_conf_parser(void) {
    TEST_SUITE("WireGuard: Configuration Parsing & Field Verification");

    const char *valid_conf =
        "[Interface]\n"
        "PrivateKey = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=\n"
        "Address = 10.8.0.5/24\n"
        "\n"
        "[Peer]\n"
        "PublicKey = bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb=\n"
        "PresharedKey = ccccccccccccccccccccccccccccccccccccccccccc=\n"
        "Endpoint = vpn.vorotabot.net:51820\n"
        "AllowedIPs = 10.8.0.0/24\n"
        "PersistentKeepalive = 30\n";

    sim_wg_config_t cfg;
    int res = sim_parse_wg_conf(valid_conf, &cfg);
    ASSERT_EQ(res, 0);
    ASSERT_STR_EQ(cfg.private_key, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=");
    ASSERT_STR_EQ(cfg.address, "10.8.0.5/24");
    ASSERT_STR_EQ(cfg.peer_public_key, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb=");
    ASSERT_STR_EQ(cfg.preshared_key, "ccccccccccccccccccccccccccccccccccccccccccc=");
    ASSERT_STR_EQ(cfg.peer_endpoint, "vpn.vorotabot.net");
    ASSERT_EQ(cfg.peer_port, 51820);
    ASSERT_STR_EQ(cfg.allowed_ips, "10.8.0.0/24");
    ASSERT_EQ(cfg.persistent_keepalive, 30);
    ASSERT_TRUE(cfg.enabled);

    // Rejection on corrupted PrivateKey
    const char *bad_priv_conf =
        "[Interface]\n"
        "PrivateKey = INVALID_KEY\n"
        "Address = 10.8.0.5/24\n"
        "[Peer]\n"
        "PublicKey = bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb=\n"
        "Endpoint = vpn.vorotabot.net:51820\n";
    res = sim_parse_wg_conf(bad_priv_conf, &cfg);
    ASSERT_EQ(res, -3);

    // Rejection on missing Endpoint
    const char *no_endp_conf =
        "[Interface]\n"
        "PrivateKey = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=\n"
        "[Peer]\n"
        "PublicKey = bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb=\n";
    res = sim_parse_wg_conf(no_endp_conf, &cfg);
    ASSERT_EQ(res, -6);
}

static void test_suite_tunnel_decision_rules(void) {
    TEST_SUITE("WireGuard: Tunnel Trust & Security Decision Matrix");

    const char *wg_ip = "10.8.0.2/24";
    const char *sta_subnet = "192.168.1.";

    // Scenario 1: Valid WireGuard peer connecting directly via tunnel
    // Local destination: 10.8.0.2, Remote peer: 10.8.0.1 (VPN gateway)
    ASSERT_TRUE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "10.8.0.1", sta_subnet));

    // Another trusted peer in the same WireGuard /24 subnet (e.g. mobile phone 10.8.0.55)
    ASSERT_TRUE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "10.8.0.55", sta_subnet));

    // Scenario 2: WireGuard client disconnected -> Untrusted
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(false, wg_ip, "10.8.0.2", "10.8.0.1", sta_subnet));

    // Scenario 3: Request addressed to Wi-Fi STA IP instead of WireGuard tunnel IP
    // Local destination: 192.168.1.50 -> Untrusted (must use password)
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "192.168.1.50", "10.8.0.1", sta_subnet));

    // Scenario 4: Remote peer outside WireGuard subnet (e.g. 10.9.0.1 or 172.16.0.5)
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "10.9.0.1", sta_subnet));
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "172.16.0.5", sta_subnet));

    // Scenario 5: Self-reflection spoofing (peer == local)
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "10.8.0.2", sta_subnet));

    // Scenario 6: SoftAP client spoofing (192.168.4.x)
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "192.168.4.2", sta_subnet));

    // Scenario 7: Wi-Fi STA LAN subnet collision spoofing
    ASSERT_FALSE(sim_eval_trusted_wg_tunnel(true, wg_ip, "10.8.0.2", "192.168.1.100", sta_subnet));
}

int main(void) {
    printf(ANSI_BOLD ">>> RUNNING SUITE: test_wireguard_rules <<<" ANSI_RESET "\n");

    test_suite_key_validation();
    test_suite_conf_parser();
    test_suite_tunnel_decision_rules();

    PRINT_TEST_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
