#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_MODE_AP_ONLY = 0,
    WIFI_MGR_MODE_STA_ONLY,
    WIFI_MGR_MODE_AP_STA
} wifi_mgr_mode_t;

typedef struct {
    char ap_ssid[32];
    char ap_password[64];
    uint8_t ap_channel;
    uint8_t ap_max_connections;
    bool ap_hidden;
    
    bool sta_enabled;
    char sta_ssid[32];
    char sta_password[64];
    bool sta_connected;
    char sta_ip[16];
} wifi_mgr_config_t;

/**
 * @brief Initialize Wi-Fi subsystem with SoftAP and optional Station support.
 * Loads configuration overrides from NVS if present.
 *
 * @param default_ssid Fallback SoftAP SSID.
 * @param default_pass Fallback SoftAP Password (or "" for open network).
 * @return ESP_OK on success.
 */
esp_err_t wifi_manager_init(const char *default_ssid, const char *default_pass);

/**
 * @brief Get current Wi-Fi configuration and runtime status.
 */
void wifi_manager_get_config(wifi_mgr_config_t *out_config);

/**
 * @brief Update SoftAP credentials in runtime and save to NVS.
 */
esp_err_t wifi_manager_set_ap_credentials(const char *ssid, const char *password);

/**
 * @brief Configure Station (STA) network to connect to home/office router.
 */
esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *password);

/**
 * @brief Disconnect STA and revert to AP-only mode.
 */
esp_err_t wifi_manager_disable_sta(void);

/**
 * @brief Get number of connected clients to SoftAP.
 */
int wifi_manager_get_ap_client_count(void);

/**
 * @brief Check if connected to an external Wi-Fi router in STA mode.
 */
bool wifi_manager_is_sta_connected(void);

/**
 * @brief Get SoftAP IP address as string (typically "192.168.4.1").
 */
const char* wifi_manager_get_ap_ip(void);

/**
 * @brief Get Station IP address string if connected.
 */
const char* wifi_manager_get_sta_ip(void);

/**
 * @brief Get STA RSSI signal strength if connected (returns 0 if disconnected).
 */
int8_t wifi_manager_get_sta_rssi(void);

#ifdef __cplusplus
}
#endif
