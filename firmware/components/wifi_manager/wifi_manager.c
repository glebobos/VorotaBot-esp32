#include "wifi_manager.h"
#include "nvs_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_MGR";

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_mgr_config_t s_config;
static SemaphoreHandle_t s_wifi_mutex = NULL;

static esp_timer_handle_t s_sta_retry_timer = NULL;
static uint32_t s_sta_retry_count = 0;
#define STA_RETRY_INTERVAL_MS 30000 // 30 seconds

// Credential verification state
static bool s_sta_pending_verification = false;
static char s_sta_prev_ssid[32] = {0};
static char s_sta_prev_pass[64] = {0};

static void wifi_lock(void) {
    if (s_wifi_mutex) {
        xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void) {
    if (s_wifi_mutex) {
        xSemaphoreGiveRecursive(s_wifi_mutex);
    }
}

static void stop_sta_retry_timer(void) {
    if (s_sta_retry_timer && esp_timer_is_active(s_sta_retry_timer)) {
        esp_timer_stop(s_sta_retry_timer);
    }
}

static void start_sta_retry_timer(void) {
    if (s_sta_retry_timer) {
        stop_sta_retry_timer();
        // Fast retry (3s) for the first 3 attempts to quickly recover from AP glitches; 30s for long-term retries
        uint32_t interval_ms = (s_sta_retry_count < 3) ? 3000 : STA_RETRY_INTERVAL_MS;
        ESP_LOGI(TAG, "Scheduling Wi-Fi STA retry in %u seconds... (attempt %u)", (unsigned int)(interval_ms / 1000), (unsigned int)(s_sta_retry_count + 1));
        esp_timer_start_once(s_sta_retry_timer, (uint64_t)interval_ms * 1000ULL);
    }
}

static void sta_retry_timer_cb(void *arg) {
    wifi_lock();
    if (s_config.sta_enabled && !s_config.sta_connected) {
        s_sta_retry_count++;
        ESP_LOGI(TAG, "Attempting Wi-Fi STA reconnection (attempt %lu)...", (unsigned long)s_sta_retry_count);

        if (s_sta_retry_count >= 5 && !s_config.ap_enabled) {
            ESP_LOGW(TAG, "STA failed to connect after %lu attempts. Enabling fallback SoftAP for recovery.", (unsigned long)s_sta_retry_count);
            wifi_manager_enable_ap();
        }

        if (s_sta_pending_verification && s_sta_retry_count >= 5) {
            ESP_LOGW(TAG, "New STA credentials failed verification! Restoring previous configuration.");
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

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect returned error: %s. Re-arming retry timer...", esp_err_to_name(err));
            start_sta_retry_timer();
        }
    }
    wifi_unlock();
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "SoftAP started successfully (SSID: %s)", s_config.ap_ssid);
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station " MACSTR " connected, AID=%d", MAC2STR(event->mac), event->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station " MACSTR " disconnected, AID=%d", MAC2STR(event->mac), event->aid);
                break;
            }
            case WIFI_EVENT_STA_START:
                wifi_lock();
                if (s_config.sta_enabled) {
                    ESP_LOGI(TAG, "Connecting to STA AP '%s'...", s_config.sta_ssid);
                    esp_wifi_connect();
                }
                wifi_unlock();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
                uint8_t reason = disconn ? disconn->reason : 0;
                wifi_lock();
                s_config.sta_connected = false;
                strcpy(s_config.sta_ip, "0.0.0.0");
                strcpy(s_config.sta_netmask, "0.0.0.0");
                ESP_LOGW(TAG, "STA disconnected (reason: %u).", reason);
                if (s_config.sta_enabled) {
                    if (s_sta_pending_verification && s_sta_retry_count >= 5) {
                        ESP_LOGW(TAG, "New STA credentials verification failed. Restoring previous state.");
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
                    if (s_sta_retry_count >= 5 && !s_config.ap_enabled) {
                        ESP_LOGW(TAG, "STA disconnected and retry threshold reached (%lu). Re-enabling SoftAP fallback.", (unsigned long)s_sta_retry_count);
                        wifi_manager_enable_ap();
                    }
                    start_sta_retry_timer();
                }
                wifi_unlock();
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            wifi_lock();
            s_config.sta_connected = true;
            s_sta_retry_count = 0;
            stop_sta_retry_timer();
            snprintf(s_config.sta_ip, sizeof(s_config.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_config.sta_netmask, sizeof(s_config.sta_netmask), IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "STA connected successfully! IP address: %s, Netmask: %s", s_config.sta_ip, s_config.sta_netmask);

            // Commit validated credentials to NVS
            if (s_sta_pending_verification) {
                nvs_manager_set_str("sta_ssid", s_config.sta_ssid);
                nvs_manager_set_str("sta_pass", s_config.sta_password);
                s_sta_pending_verification = false;
                ESP_LOGI(TAG, "Verified and committed new STA credentials to NVS");
            }

            // Once external Wi-Fi connects successfully, disable SoftAP if active
            if (s_config.ap_enabled) {
                ESP_LOGI(TAG, "External Wi-Fi connected and operational. Disabling SoftAP.");
                s_config.ap_enabled = false;
                esp_wifi_set_mode(WIFI_MODE_STA);
            }
            wifi_unlock();
        }
    }
}

esp_err_t wifi_manager_init(const char *default_ssid, const char *default_pass) {
    if (!s_wifi_mutex) {
        s_wifi_mutex = xSemaphoreCreateRecursiveMutex();
    }
    wifi_lock();
    memset(&s_config, 0, sizeof(s_config));
    
    // Load config from NVS or fallback to defaults
    nvs_manager_get_str("ap_ssid", s_config.ap_ssid, sizeof(s_config.ap_ssid), default_ssid ? default_ssid : "ESP32-Device");
    nvs_manager_get_str("ap_pass", s_config.ap_password, sizeof(s_config.ap_password), default_pass ? default_pass : "12345678");
    
    int32_t channel = 1;
    nvs_manager_get_i32("ap_chan", &channel, 1);
    s_config.ap_channel = (uint8_t)channel;
    s_config.ap_max_connections = 6;
    s_config.ap_hidden = false;
    
    nvs_manager_get_str("sta_ssid", s_config.sta_ssid, sizeof(s_config.sta_ssid), "");
    nvs_manager_get_str("sta_pass", s_config.sta_password, sizeof(s_config.sta_password), "");
    s_config.sta_enabled = (strlen(s_config.sta_ssid) > 0);
    s_config.sta_connected = false;
    strcpy(s_config.sta_ip, "0.0.0.0");
    strcpy(s_config.sta_netmask, "0.0.0.0");

    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create SoftAP netif");
        wifi_unlock();
        return ESP_FAIL;
    }
    
    // Configure default SoftAP static IP: 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    // Create STA netif
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    if (s_config.sta_enabled) {
        // External Wi-Fi is configured: disable SoftAP by default, enable STA only
        s_config.ap_enabled = false;
        ESP_LOGI(TAG, "External Wi-Fi configured (SSID: '%s'). SoftAP disabled by default, initializing STA mode.", s_config.sta_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        
        wifi_config_t wifi_sta_config;
        memset(&wifi_sta_config, 0, sizeof(wifi_sta_config));
        snprintf((char*)wifi_sta_config.sta.ssid, sizeof(wifi_sta_config.sta.ssid), "%s", s_config.sta_ssid);
        snprintf((char*)wifi_sta_config.sta.password, sizeof(wifi_sta_config.sta.password), "%s", s_config.sta_password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    } else {
        // External Wi-Fi is not configured: enable SoftAP mode
        s_config.ap_enabled = true;
        ESP_LOGI(TAG, "External Wi-Fi not configured. Enabling SoftAP mode (SSID: '%s').", s_config.ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

        wifi_config_t wifi_ap_config = {
            .ap = {
                .channel = s_config.ap_channel,
                .max_connection = s_config.ap_max_connections,
                .authmode = (strlen(s_config.ap_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
                .ssid_hidden = s_config.ap_hidden ? 1 : 0,
            },
        };
        snprintf((char*)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), "%s", s_config.ap_ssid);
        wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
        snprintf((char*)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s", s_config.ap_password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    }

    if (s_sta_retry_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &sta_retry_timer_cb,
            .name = "sta_retry_tmr"
        };
        esp_err_t tmr_err = esp_timer_create(&timer_args, &s_sta_retry_timer);
        if (tmr_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create STA retry timer: %s", esp_err_to_name(tmr_err));
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_unlock();
    return ESP_OK;
}

void wifi_manager_get_config(wifi_mgr_config_t *out_config) {
    if (out_config) {
        wifi_lock();
        memcpy(out_config, &s_config, sizeof(wifi_mgr_config_t));
        wifi_unlock();
    }
}

esp_err_t wifi_manager_set_ap_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    
    wifi_lock();
    nvs_manager_set_str("ap_ssid", ssid);
    if (password) {
        nvs_manager_set_str("ap_pass", password);
    }
    
    snprintf(s_config.ap_ssid, sizeof(s_config.ap_ssid), "%s", ssid);
    if (password) {
        snprintf(s_config.ap_password, sizeof(s_config.ap_password), "%s", password);
    }
    
    if (s_config.ap_enabled) {
        wifi_config_t wifi_ap_config;
        esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &wifi_ap_config);
        if (err == ESP_OK) {
            snprintf((char*)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), "%s", s_config.ap_ssid);
            wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
            if (password) {
                snprintf((char*)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s", s_config.ap_password);
                wifi_ap_config.ap.authmode = (strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
            }
            esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
            wifi_unlock();
            return ret;
        }
    }
    wifi_unlock();
    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    
    wifi_lock();
    stop_sta_retry_timer();
    s_sta_retry_count = 0;

    // Cache previous credentials in case new ones fail verification
    snprintf(s_sta_prev_ssid, sizeof(s_sta_prev_ssid), "%s", s_config.sta_ssid);
    snprintf(s_sta_prev_pass, sizeof(s_sta_prev_pass), "%s", s_config.sta_password);
    s_sta_pending_verification = true;

    // Apply credentials to runtime struct
    snprintf(s_config.sta_ssid, sizeof(s_config.sta_ssid), "%s", ssid);
    snprintf(s_config.sta_password, sizeof(s_config.sta_password), "%s", password ? password : "");
    s_config.sta_enabled = true;
    s_config.sta_connected = false;
    strcpy(s_config.sta_ip, "0.0.0.0");
    strcpy(s_config.sta_netmask, "0.0.0.0");

    // Keep or re-enable SoftAP in APSTA mode so caller retains access during connection attempt
    if (!s_config.ap_enabled) {
        s_config.ap_enabled = true;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifi_config_t wifi_ap_config = {
            .ap = {
                .channel = s_config.ap_channel,
                .max_connection = s_config.ap_max_connections,
                .authmode = (strlen(s_config.ap_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
                .ssid_hidden = s_config.ap_hidden ? 1 : 0,
            },
        };
        snprintf((char*)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), "%s", s_config.ap_ssid);
        wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
        snprintf((char*)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s", s_config.ap_password);
        esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
    } else {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    
    wifi_config_t wifi_sta_config;
    memset(&wifi_sta_config, 0, sizeof(wifi_sta_config));
    snprintf((char*)wifi_sta_config.sta.ssid, sizeof(wifi_sta_config.sta.ssid), "%s", s_config.sta_ssid);
    snprintf((char*)wifi_sta_config.sta.password, sizeof(wifi_sta_config.sta.password), "%s", s_config.sta_password);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config);
    
    esp_err_t ret = esp_wifi_connect();
    wifi_unlock();
    return ret;
}

esp_err_t wifi_manager_disable_sta(void) {
    wifi_lock();
    stop_sta_retry_timer();
    s_sta_retry_count = 0;
    s_sta_pending_verification = false;

    nvs_manager_erase_key("sta_ssid");
    nvs_manager_erase_key("sta_pass");
    
    s_config.sta_enabled = false;
    s_config.sta_connected = false;
    s_config.sta_ssid[0] = '\0';
    s_config.sta_password[0] = '\0';
    strcpy(s_config.sta_ip, "0.0.0.0");
    strcpy(s_config.sta_netmask, "0.0.0.0");
    
    esp_wifi_disconnect();

    // External Wi-Fi is no longer configured: enable SoftAP mode
    ESP_LOGI(TAG, "External Wi-Fi disabled/erased. Enabling SoftAP mode (SSID: '%s').", s_config.ap_ssid);
    s_config.ap_enabled = true;
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t wifi_ap_config = {
        .ap = {
            .channel = s_config.ap_channel,
            .max_connection = s_config.ap_max_connections,
            .authmode = (strlen(s_config.ap_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .ssid_hidden = s_config.ap_hidden ? 1 : 0,
        },
    };
    snprintf((char*)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), "%s", s_config.ap_ssid);
    wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
    snprintf((char*)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s", s_config.ap_password);
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
    wifi_unlock();
    return ret;
}

bool wifi_manager_is_ap_enabled(void) {
    wifi_lock();
    bool enabled = s_config.ap_enabled;
    wifi_unlock();
    return enabled;
}

esp_err_t wifi_manager_enable_ap(void) {
    wifi_lock();
    if (s_config.ap_enabled) {
        wifi_unlock();
        return ESP_OK;
    }

    s_config.ap_enabled = true;
    wifi_mode_t target_mode = s_config.sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(target_mode);
    if (err != ESP_OK) {
        wifi_unlock();
        return err;
    }

    wifi_config_t wifi_ap_config = {
        .ap = {
            .channel = s_config.ap_channel,
            .max_connection = s_config.ap_max_connections,
            .authmode = (strlen(s_config.ap_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .ssid_hidden = s_config.ap_hidden ? 1 : 0,
        },
    };
    snprintf((char*)wifi_ap_config.ap.ssid, sizeof(wifi_ap_config.ap.ssid), "%s", s_config.ap_ssid);
    wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
    snprintf((char*)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s", s_config.ap_password);
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
    wifi_unlock();
    return err;
}

esp_err_t wifi_manager_disable_ap(void) {
    wifi_lock();
    if (!s_config.ap_enabled) {
        wifi_unlock();
        return ESP_OK;
    }

    // Safety interlock: prevent turning off SoftAP if STA is not currently connected
    if (!s_config.sta_connected) {
        ESP_LOGW(TAG, "Cannot disable SoftAP: Station is not connected to a network. Keeping AP active to prevent lockout.");
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_config.ap_enabled = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_unlock();
    return err;
}

int wifi_manager_get_ap_client_count(void) {
    wifi_lock();
    if (!s_config.ap_enabled) {
        wifi_unlock();
        return 0;
    }
    wifi_unlock();
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

bool wifi_manager_is_sta_connected(void) {
    wifi_lock();
    bool connected = s_config.sta_connected;
    wifi_unlock();
    return connected;
}

const char* wifi_manager_get_ap_ip(void) {
    return "192.168.4.1";
}

const char* wifi_manager_get_sta_ip(void) {
    wifi_lock();
    static char s_out_sta_ip[16];
    snprintf(s_out_sta_ip, sizeof(s_out_sta_ip), "%s", s_config.sta_ip);
    wifi_unlock();
    return s_out_sta_ip;
}

int8_t wifi_manager_get_sta_rssi(void) {
    wifi_lock();
    if (!s_config.sta_connected) {
        wifi_unlock();
        return 0;
    }
    wifi_unlock();
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_manager_is_in_sta_subnet(const char *ip_str) {
    if (!ip_str) return false;
    wifi_lock();
    if (!s_config.sta_connected || strlen(s_config.sta_ip) == 0 || strcmp(s_config.sta_ip, "0.0.0.0") == 0) {
        wifi_unlock();
        return false;
    }
    ip4_addr_t peer_ip, sta_ip, netmask;
    if (ip4addr_aton(ip_str, &peer_ip) != 1 || ip4addr_aton(s_config.sta_ip, &sta_ip) != 1) {
        wifi_unlock();
        return false;
    }
    if (strlen(s_config.sta_netmask) > 0 && strcmp(s_config.sta_netmask, "0.0.0.0") != 0) {
        if (ip4addr_aton(s_config.sta_netmask, &netmask) != 1) {
            ip4addr_aton("255.255.255.0", &netmask);
        }
    } else {
        ip4addr_aton("255.255.255.0", &netmask);
    }
    bool in_subnet = ((peer_ip.addr & netmask.addr) == (sta_ip.addr & netmask.addr));
    wifi_unlock();
    return in_subnet;
}
