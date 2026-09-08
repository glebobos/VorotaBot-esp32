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
static bool s_config_updated = false;

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

    ESP_LOGI(TAG, "WireGuard config loaded: configured=%s, enabled=%s, endpoint=%s:%u, ip=%s",
             configured ? "YES" : "NO", s_config.enabled ? "YES" : "NO",
             s_config.peer_endpoint, s_config.peer_port, s_config.address);

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

    bool logged_not_configured = false;
    bool logged_disabled = false;
    bool logged_waiting_wifi = false;

    while (1) {
        // 1. Wait until Wi-Fi is connected, WireGuard is enabled and configured
        while (1) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool wifi_up = s_wifi_connected;
            bool configured = s_status.is_configured;
            bool enabled = s_config.enabled;
            s_status.is_connected = false;
            s_status.uptime_seconds = 0;
            xSemaphoreGive(s_lock);

            if (wifi_up && configured && enabled) {
                break;
            }

            if (!configured) {
                if (!logged_not_configured) {
                    ESP_LOGW(TAG, "WireGuard not configured (keys/endpoint missing in NVS). Set config via web portal or provision NVS.");
                    logged_not_configured = true;
                }
            } else if (!enabled) {
                if (!logged_disabled) {
                    ESP_LOGW(TAG, "WireGuard administratively disabled.");
                    logged_disabled = true;
                }
            } else if (!wifi_up) {
                if (!logged_waiting_wifi) {
                    ESP_LOGI(TAG, "WireGuard waiting for Wi-Fi connection...");
                    logged_waiting_wifi = true;
                }
            }

            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        }

        logged_waiting_wifi = false;
        logged_not_configured = false;
        logged_disabled = false;

        // 2. Ensure system clock is valid (WireGuard requires accurate timestamps)
        wait_for_sntp();

        // 3. Take snapshot of configuration under lock to prevent torn reads
        wg_manager_config_t cfg;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(&cfg, &s_config, sizeof(wg_manager_config_t));
        s_config_updated = false;
        xSemaphoreGive(s_lock);

        char clean_ip[32];
        snprintf(clean_ip, sizeof(clean_ip), "%s", cfg.address);
        char *slash = strchr(clean_ip, '/');
        if (slash) *slash = '\0';

        // 4. Initialize WireGuard network interface
        wireguard_config_t wg_cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
        wg_cfg.private_key = cfg.private_key;
        wg_cfg.public_key = cfg.peer_public_key;
        if (strlen(cfg.preshared_key) > 0) {
            wg_cfg.preshared_key = cfg.preshared_key;
        } else {
            wg_cfg.preshared_key = NULL;
        }
        wg_cfg.endpoint = cfg.peer_endpoint;
        wg_cfg.port = cfg.peer_port;
        wg_cfg.persistent_keepalive = cfg.persistent_keepalive ? cfg.persistent_keepalive : 25;
        wg_cfg.allowed_ip = clean_ip;
        wg_cfg.allowed_ip_mask = "255.255.255.0";

        ESP_LOGI(TAG, "Initializing WireGuard network interface (%s/24 -> %s:%u)...",
                 clean_ip, cfg.peer_endpoint, cfg.peer_port);

        esp_err_t err = esp_wireguard_init(&wg_cfg, &wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_init failed: %s. Retrying in 5s...", esp_err_to_name(err));
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }

        err = esp_wireguard_connect(&wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_connect failed: %s. Retrying in 5s...", esp_err_to_name(err));
            esp_wireguard_disconnect(&wg_ctx);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "WireGuard interface created. Waiting for peer handshake (up to 15s)...");

        // 5. Wait for peer handshake to verify link
        int wait_sec = 0;
        bool peer_up = false;
        while (wait_sec++ < 15) {
            bool abort_wait = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_wifi_connected || !s_config.enabled || s_config_updated) {
                abort_wait = true;
            }
            xSemaphoreGive(s_lock);
            if (abort_wait) {
                break;
            }

            if (esp_wireguardif_peer_is_up(&wg_ctx) == ESP_OK) {
                peer_up = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (!peer_up) {
            ESP_LOGW(TAG, "WireGuard peer handshake timed out (endpoint %s:%u unreachable). Retrying in 10s...",
                     cfg.peer_endpoint, cfg.peer_port);
            esp_wireguard_disconnect(&wg_ctx);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.is_connected = false;
            s_status.uptime_seconds = 0;
            xSemaphoreGive(s_lock);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "WireGuard peer handshake verified! Interface UP at %s", clean_ip);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.is_connected = true;
        snprintf(s_status.assigned_ip, sizeof(s_status.assigned_ip), "%s", clean_ip);
        s_status.last_handshake_epoch = (uint32_t)time(NULL);
        xSemaphoreGive(s_lock);

        // Trigger connected callback (Route 53 DNS sync guard)
        if (s_on_connected_cb) {
            s_on_connected_cb(clean_ip);
        }

        // 6. Active monitor loop while active (health checks every 5 seconds)
        uint32_t uptime = 0;
        int missed_handshake_count = 0;
        const int MAX_MISSED_CHECKS = 12; // 12 * 5s = 60 seconds without peer response

        while (1) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

            bool should_exit = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_wifi_connected || !s_config.enabled || s_config_updated) {
                should_exit = true;
            }
            xSemaphoreGive(s_lock);

            if (should_exit) {
                break;
            }

            uptime += 5;
            bool up = (esp_wireguardif_peer_is_up(&wg_ctx) == ESP_OK);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.uptime_seconds = uptime;
            if (up) {
                missed_handshake_count = 0;
                s_status.last_handshake_epoch = (uint32_t)time(NULL);
                s_status.is_connected = true;
            } else {
                missed_handshake_count++;
                s_status.is_connected = false;
            }
            xSemaphoreGive(s_lock);

            if (missed_handshake_count >= MAX_MISSED_CHECKS) {
                ESP_LOGW(TAG, "WireGuard peer unreachable for %d seconds. Rebuilding tunnel...",
                         missed_handshake_count * 5);
                break;
            }
        }

        // 7. Cleanup: tear down WireGuard interface before re-creating or idling
        ESP_LOGI(TAG, "Tearing down WireGuard tunnel interface...");
        esp_wireguard_disconnect(&wg_ctx);

        bool should_cooldown = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.is_connected = false;
        s_status.uptime_seconds = 0;
        should_cooldown = (s_wifi_connected && s_config.enabled && !s_config_updated);
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "WireGuard monitor loop exited (Wi-Fi connected: %s, Enabled: %s)",
                 s_wifi_connected ? "YES" : "NO", s_config.enabled ? "YES" : "NO");

        if (should_cooldown) {
            ESP_LOGI(TAG, "WireGuard link dropped. Reconnecting in 5 seconds...");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        }
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

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

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
    s_config_updated = true;
    xSemaphoreGive(s_lock);

    if (s_wg_task_handle) {
        xTaskNotifyGive(s_wg_task_handle);
    }

    ESP_LOGI(TAG, "WireGuard configuration updated in NVS");
    return ESP_OK;
}

esp_err_t wireguard_manager_set_config_from_text(const char *conf_text) {
    if (!conf_text || strlen(conf_text) == 0) return ESP_ERR_INVALID_ARG;

    wg_manager_config_t parsed = {0};
    parsed.peer_port = 443;
    parsed.persistent_keepalive = 25;
    parsed.enabled = true;
    strncpy(parsed.address, "10.0.0.2", sizeof(parsed.address) - 1);
    strncpy(parsed.allowed_ips, "0.0.0.0/0", sizeof(parsed.allowed_ips) - 1);

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
                char *colon = strrchr(val, ':');
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
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_config, &s_config, sizeof(wg_manager_config_t));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void wireguard_manager_get_status(wireguard_status_t *out_status) {
    if (!out_status) return;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_status, &s_status, sizeof(wireguard_status_t));
    xSemaphoreGive(s_lock);
}

esp_err_t wireguard_manager_set_enabled(bool enabled) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config.enabled = enabled;
    s_status.is_enabled = enabled;
    nvs_manager_set_i32("wg_en", enabled ? 1 : 0);
    xSemaphoreGive(s_lock);
    if (s_wg_task_handle) {
        xTaskNotifyGive(s_wg_task_handle);
    }
    ESP_LOGI(TAG, "WireGuard administratively %s", enabled ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

void wireguard_manager_on_wifi_connected(void) {
    ESP_LOGI(TAG, "Wi-Fi Station online -> resuming WireGuard operations");
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_connected = true;
    xSemaphoreGive(s_lock);
    if (s_wg_task_handle) {
        xTaskNotifyGive(s_wg_task_handle);
    }
}

void wireguard_manager_on_wifi_disconnected(void) {
    ESP_LOGW(TAG, "Wi-Fi Station offline -> pausing WireGuard tunnel");
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_connected = false;
    s_status.is_connected = false;
    xSemaphoreGive(s_lock);
    if (s_wg_task_handle) {
        xTaskNotifyGive(s_wg_task_handle);
    }
}

