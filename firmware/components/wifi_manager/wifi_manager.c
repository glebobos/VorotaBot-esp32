#include "wifi_manager.h"
#include "nvs_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_mgr_config_t s_config;
static esp_timer_handle_t s_sta_retry_timer = NULL;
static uint32_t s_sta_retry_count = 0;
#define STA_RETRY_INTERVAL_MS 30000 // 30 seconds

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
    if (s_config.sta_enabled && !s_config.sta_connected) {
        s_sta_retry_count++;
        ESP_LOGI(TAG, "Attempting Wi-Fi STA reconnection (attempt %lu)...", (unsigned long)s_sta_retry_count);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect returned error: %s. Re-arming retry timer...", esp_err_to_name(err));
            start_sta_retry_timer();
        }
    }
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
                if (s_config.sta_enabled) {
                    ESP_LOGI(TAG, "Connecting to STA AP '%s'...", s_config.sta_ssid);
                    esp_wifi_connect();
                }
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
                uint8_t reason = disconn ? disconn->reason : 0;
                s_config.sta_connected = false;
                strcpy(s_config.sta_ip, "0.0.0.0");
                ESP_LOGW(TAG, "STA disconnected (reason: %u).", reason);
                if (s_config.sta_enabled) {
                    start_sta_retry_timer();
                }
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            s_config.sta_connected = true;
            s_sta_retry_count = 0;
            stop_sta_retry_timer();
            snprintf(s_config.sta_ip, sizeof(s_config.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA connected successfully! IP address: %s", s_config.sta_ip);
        }
    }
}

esp_err_t wifi_manager_init(const char *default_ssid, const char *default_pass) {
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

    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create SoftAP netif");
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

    // Create STA netif if STA is enabled
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .channel = s_config.ap_channel,
            .max_connection = s_config.ap_max_connections,
            .authmode = (strlen(s_config.ap_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .ssid_hidden = s_config.ap_hidden ? 1 : 0,
        },
    };
    strncpy((char*)wifi_ap_config.ap.ssid, s_config.ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
    strncpy((char*)wifi_ap_config.ap.password, s_config.ap_password, sizeof(wifi_ap_config.ap.password));

    if (s_config.sta_enabled) {
        ESP_LOGI(TAG, "Initializing AP+STA mode (AP: '%s', STA: '%s')", s_config.ap_ssid, s_config.sta_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
        
        wifi_config_t wifi_sta_config;
        memset(&wifi_sta_config, 0, sizeof(wifi_sta_config));
        strncpy((char*)wifi_sta_config.sta.ssid, s_config.sta_ssid, sizeof(wifi_sta_config.sta.ssid));
        strncpy((char*)wifi_sta_config.sta.password, s_config.sta_password, sizeof(wifi_sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    } else {
        ESP_LOGI(TAG, "Initializing SoftAP mode only (SSID: '%s')", s_config.ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
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
    return ESP_OK;
}

void wifi_manager_get_config(wifi_mgr_config_t *out_config) {
    if (out_config) {
        memcpy(out_config, &s_config, sizeof(wifi_mgr_config_t));
    }
}

esp_err_t wifi_manager_set_ap_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    
    nvs_manager_set_str("ap_ssid", ssid);
    if (password) {
        nvs_manager_set_str("ap_pass", password);
    }
    
    strncpy(s_config.ap_ssid, ssid, sizeof(s_config.ap_ssid) - 1);
    if (password) {
        strncpy(s_config.ap_password, password, sizeof(s_config.ap_password) - 1);
    }
    
    wifi_config_t wifi_ap_config;
    esp_wifi_get_config(WIFI_IF_AP, &wifi_ap_config);
    strncpy((char*)wifi_ap_config.ap.ssid, s_config.ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    wifi_ap_config.ap.ssid_len = strlen(s_config.ap_ssid);
    if (password) {
        strncpy((char*)wifi_ap_config.ap.password, s_config.ap_password, sizeof(wifi_ap_config.ap.password));
        wifi_ap_config.ap.authmode = (strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
}

esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    
    stop_sta_retry_timer();
    s_sta_retry_count = 0;

    nvs_manager_set_str("sta_ssid", ssid);
    nvs_manager_set_str("sta_pass", password ? password : "");
    
    strncpy(s_config.sta_ssid, ssid, sizeof(s_config.sta_ssid) - 1);
    strncpy(s_config.sta_password, password ? password : "", sizeof(s_config.sta_password) - 1);
    s_config.sta_enabled = true;
    
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    
    wifi_config_t wifi_sta_config;
    memset(&wifi_sta_config, 0, sizeof(wifi_sta_config));
    strncpy((char*)wifi_sta_config.sta.ssid, s_config.sta_ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char*)wifi_sta_config.sta.password, s_config.sta_password, sizeof(wifi_sta_config.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config);
    
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disable_sta(void) {
    stop_sta_retry_timer();
    s_sta_retry_count = 0;

    nvs_manager_erase_key("sta_ssid");
    nvs_manager_erase_key("sta_pass");
    
    s_config.sta_enabled = false;
    s_config.sta_connected = false;
    s_config.sta_ssid[0] = '\0';
    s_config.sta_password[0] = '\0';
    strcpy(s_config.sta_ip, "0.0.0.0");
    
    esp_wifi_disconnect();
    return esp_wifi_set_mode(WIFI_MODE_AP);
}

int wifi_manager_get_ap_client_count(void) {
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

bool wifi_manager_is_sta_connected(void) {
    return s_config.sta_connected;
}

const char* wifi_manager_get_ap_ip(void) {
    return "192.168.4.1";
}

const char* wifi_manager_get_sta_ip(void) {
    return s_config.sta_ip;
}

int8_t wifi_manager_get_sta_rssi(void) {
    if (!s_config.sta_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}
