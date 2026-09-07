#include "wireguard_manager.h"
#include "nvs_manager.h"
#include "esp_wireguard.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WG_MGR";

static wg_manager_config_t s_config;
static wireguard_status_t s_status;
static SemaphoreHandle_t s_lock = NULL;
static wg_connected_cb_t s_on_connected_cb = NULL;
static TaskHandle_t s_wg_task_handle = NULL;
static bool s_sntp_initialized = false;
static bool s_wifi_connected = false;

static void trim_whitespace(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

static esp_err_t load_config_from_nvs(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_config, 0, sizeof(s_config));

    nvs_manager_get_str("wg_priv", s_config.private_key, sizeof(s_config.private_key), "");
    nvs_manager_get_str("wg_addr", s_config.address, sizeof(s_config.address), "10.0.0.2");
    nvs_manager_get_str("wg_peer_pub", s_config.peer_public_key, sizeof(s_config.peer_public_key), "");
    nvs_manager_get_str("wg_psk", s_config.preshared_key, sizeof(s_config.preshared_key), "");
    nvs_manager_get_str("wg_endp", s_config.peer_endpoint, sizeof(s_config.peer_endpoint), "");

    int32_t port = 443;
    nvs_manager_get_i32("wg_port", &port, 443);
    s_config.peer_port = (uint16_t)port;

    nvs_manager_get_str("wg_allow", s_config.allowed_ips, sizeof(s_config.allowed_ips), "0.0.0.0/0");

    int32_t keepalive = 25;
    nvs_manager_get_i32("wg_ka", &keepalive, 25);
    s_config.persistent_keepalive = (uint16_t)keepalive;

    int32_t en = 1;
    nvs_manager_get_i32("wg_en", &en, 1);
    s_config.enabled = (en != 0);

    bool configured = (strlen(s_config.private_key) > 0 && strlen(s_config.peer_public_key) > 0 && strlen(s_config.peer_endpoint) > 0);
    s_status.is_configured = configured;
    s_status.is_enabled = s_config.enabled;
    snprintf(s_status.endpoint, sizeof(s_status.endpoint), "%s", s_config.peer_endpoint);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void wait_for_sntp(void) {
    if (!s_sntp_initialized) {
        ESP_LOGI(TAG, "Initializing SNTP for WireGuard time synchronization...");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        s_sntp_initialized = true;
    }

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int max_retry = 15;

    while (retry++ < max_retry) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2024 - 1900)) {
            ESP_LOGI(TAG, "Time successfully synchronized via SNTP: %s", asctime(&timeinfo));
            return;
        }
        ESP_LOGI(TAG, "Waiting for system time sync... (%d/%d)", retry, max_retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SNTP time synchronization timed out. Proceeding with caution.");
}

static void wireguard_tunnel_task(void *pvParameters) {
    ESP_LOGI(TAG, "WireGuard tunnel management task started");
    wireguard_ctx_t wg_ctx = {0};
    bool is_connected = false;

    while (1) {
        // Wait until Wi-Fi is connected and tunnel is enabled and configured
        while (!s_wifi_connected || !s_config.enabled || !s_status.is_configured) {
            if (is_connected) {
                ESP_LOGI(TAG, "Disconnecting WireGuard tunnel...");
                esp_wireguard_disconnect(&wg_ctx);
                is_connected = false;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.is_connected = false;
                s_status.uptime_seconds = 0;
                xSemaphoreGive(s_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // 1. Ensure system clock is valid
        wait_for_sntp();

        // 2. Prepare WireGuard configuration
        char clean_ip[32];
        snprintf(clean_ip, sizeof(clean_ip), "%s", s_config.address);
        char *slash = strchr(clean_ip, '/');
        if (slash) *slash = '\0';

        wireguard_config_t wg_cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
        wg_cfg.private_key = s_config.private_key;
        wg_cfg.public_key = s_config.peer_public_key;
        if (strlen(s_config.preshared_key) > 0) {
            wg_cfg.preshared_key = s_config.preshared_key;
        } else {
            wg_cfg.preshared_key = NULL;
        }
        wg_cfg.endpoint = s_config.peer_endpoint;
        wg_cfg.port = s_config.peer_port;
        wg_cfg.persistent_keepalive = s_config.persistent_keepalive ? s_config.persistent_keepalive : 25;
        wg_cfg.allowed_ip = clean_ip;
        wg_cfg.allowed_ip_mask = "255.255.255.0";

        ESP_LOGI(TAG, "Initializing WireGuard network interface (%s/24 -> %s:%u)...",
                 clean_ip, s_config.peer_endpoint, s_config.peer_port);

        esp_err_t err = esp_wireguard_init(&wg_cfg, &wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_init failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        err = esp_wireguard_connect(&wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_connect failed: %s", esp_err_to_name(err));
            esp_wireguard_disconnect(&wg_ctx);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        is_connected = true;
        ESP_LOGI(TAG, "WireGuard interface created, waiting for peer handshake...");

        // 3. Wait for peer handshake to verify link
        int wait_sec = 0;
        bool peer_up = false;
        while (s_wifi_connected && s_config.enabled && wait_sec++ < 20) {
            if (esp_wireguardif_peer_is_up(&wg_ctx) == ESP_OK) {
                peer_up = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (peer_up) {
            ESP_LOGI(TAG, "WireGuard peer handshake verified! Interface UP at %s", clean_ip);
        } else {
            ESP_LOGW(TAG, "Peer handshake timeout; keeping interface active");
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.is_connected = true;
        snprintf(s_status.assigned_ip, sizeof(s_status.assigned_ip), "%s", clean_ip);
        s_status.last_handshake_epoch = (uint32_t)time(NULL);
        xSemaphoreGive(s_lock);

        // Trigger connected callback (Route 53 DNS update)
        if (s_on_connected_cb) {
            s_on_connected_cb(clean_ip);
        }

        // 4. Monitor loop while active
        uint32_t uptime = 0;
        while (s_wifi_connected && s_config.enabled) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            uptime += 5;
            bool up = (esp_wireguardif_peer_is_up(&wg_ctx) == ESP_OK);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.uptime_seconds = uptime;
            if (up) {
                s_status.last_handshake_epoch = (uint32_t)time(NULL);
                s_status.is_connected = true;
            }
            xSemaphoreGive(s_lock);
        }

        // 5. Cleanup when tunnel disconnected or disabled
        if (is_connected) {
            esp_wireguard_disconnect(&wg_ctx);
            is_connected = false;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.is_connected = false;
        s_status.uptime_seconds = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "WireGuard tunnel disconnected");
    }
}

esp_err_t wireguard_manager_init(wg_connected_cb_t on_connected_cb) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_on_connected_cb = on_connected_cb;

    load_config_from_nvs();

    if (s_wg_task_handle == NULL) {
        xTaskCreate(wireguard_tunnel_task, "wg_tunnel_task", 8192, NULL, 4, &s_wg_task_handle);
    }

    return ESP_OK;
}

esp_err_t wireguard_manager_set_config(const wg_manager_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_config, config, sizeof(wg_manager_config_t));

    nvs_manager_set_str("wg_priv", s_config.private_key);
    nvs_manager_set_str("wg_addr", s_config.address);
    nvs_manager_set_str("wg_peer_pub", s_config.peer_public_key);
    nvs_manager_set_str("wg_psk", s_config.preshared_key);
    nvs_manager_set_str("wg_endp", s_config.peer_endpoint);
    nvs_manager_set_i32("wg_port", (int32_t)s_config.peer_port);
    nvs_manager_set_str("wg_allow", s_config.allowed_ips);
    nvs_manager_set_i32("wg_ka", (int32_t)s_config.persistent_keepalive);
    nvs_manager_set_i32("wg_en", s_config.enabled ? 1 : 0);

    s_status.is_configured = (strlen(s_config.private_key) > 0 && strlen(s_config.peer_public_key) > 0 && strlen(s_config.peer_endpoint) > 0);
    s_status.is_enabled = s_config.enabled;
    snprintf(s_status.endpoint, sizeof(s_status.endpoint), "%s", s_config.peer_endpoint);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "WireGuard configuration updated in NVS");
    return ESP_OK;
}

esp_err_t wireguard_manager_set_config_from_text(const char *conf_text) {
    if (!conf_text || strlen(conf_text) == 0) return ESP_ERR_INVALID_ARG;

    wg_manager_config_t parsed = {0};
    parsed.peer_port = 443;
    parsed.persistent_keepalive = 25;
    parsed.enabled = true;
    snprintf(parsed.allowed_ips, sizeof(parsed.allowed_ips), "%s", "0.0.0.0/0");

    char *copy = strdup(conf_text);
    if (!copy) return ESP_ERR_NO_MEM;

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
                snprintf(parsed.private_key, sizeof(parsed.private_key), "%s", val);
            } else if (strcasecmp(key, "Address") == 0) {
                snprintf(parsed.address, sizeof(parsed.address), "%s", val);
            } else if (strcasecmp(key, "PublicKey") == 0) {
                snprintf(parsed.peer_public_key, sizeof(parsed.peer_public_key), "%s", val);
            } else if (strcasecmp(key, "PresharedKey") == 0) {
                snprintf(parsed.preshared_key, sizeof(parsed.preshared_key), "%s", val);
            } else if (strcasecmp(key, "Endpoint") == 0) {
                char *colon = strchr(val, ':');
                if (colon) {
                    *colon = '\0';
                    snprintf(parsed.peer_endpoint, sizeof(parsed.peer_endpoint), "%s", val);
                    parsed.peer_port = (uint16_t)atoi(colon + 1);
                } else {
                    snprintf(parsed.peer_endpoint, sizeof(parsed.peer_endpoint), "%s", val);
                }
            } else if (strcasecmp(key, "AllowedIPs") == 0) {
                snprintf(parsed.allowed_ips, sizeof(parsed.allowed_ips), "%s", val);
            } else if (strcasecmp(key, "PersistentKeepalive") == 0) {
                parsed.persistent_keepalive = (uint16_t)atoi(val);
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(copy);

    ESP_LOGI(TAG, "Parsed WireGuard config: Address=%s, Endpoint=%s:%u, PeerPub=%s",
             parsed.address, parsed.peer_endpoint, parsed.peer_port, parsed.peer_public_key);

    return wireguard_manager_set_config(&parsed);
}

esp_err_t wireguard_manager_get_config(wg_manager_config_t *out_config) {
    if (!out_config) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_config, &s_config, sizeof(wg_manager_config_t));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void wireguard_manager_get_status(wireguard_status_t *out_status) {
    if (!out_status) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_status, &s_status, sizeof(wireguard_status_t));
    xSemaphoreGive(s_lock);
}

esp_err_t wireguard_manager_set_enabled(bool enabled) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config.enabled = enabled;
    s_status.is_enabled = enabled;
    nvs_manager_set_i32("wg_en", enabled ? 1 : 0);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "WireGuard administratively %s", enabled ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

void wireguard_manager_on_wifi_connected(void) {
    s_wifi_connected = true;
}

void wireguard_manager_on_wifi_disconnected(void) {
    s_wifi_connected = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.is_connected = false;
    xSemaphoreGive(s_lock);
}
