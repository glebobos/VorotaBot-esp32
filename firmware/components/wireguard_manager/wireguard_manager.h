#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char private_key[64];       /**< Client interface private key */
    char address[32];           /**< Client interface address (e.g. 10.0.0.7) */
    char peer_public_key[64];   /**< Server peer public key */
    char preshared_key[64];     /**< Server peer preshared key (optional) */
    char peer_endpoint[128];    /**< Server endpoint hostname or IP */
    uint16_t peer_port;         /**< Server UDP port (e.g. 443 or 51820) */
    char allowed_ips[64];       /**< Allowed IPs (e.g. 0.0.0.0/0 or 10.0.0.0/24) */
    uint16_t persistent_keepalive; /**< Keepalive seconds (e.g. 25) */
    bool enabled;               /**< Tunnel administratively enabled */
} wg_manager_config_t;

typedef struct {
    bool is_configured;
    bool is_enabled;
    bool is_connected;
    char assigned_ip[32];
    char endpoint[128];
    uint32_t last_handshake_epoch;
    uint32_t uptime_seconds;
} wireguard_status_t;

typedef void (*wg_connected_cb_t)(const char *tunnel_ip);

/**
 * @brief Initialize WireGuard manager, load configuration from NVS, and register callbacks.
 *
 * @param on_connected_cb Callback executed when tunnel establishes and acquires IP.
 * @return ESP_OK on success.
 */
esp_err_t wireguard_manager_init(wg_connected_cb_t on_connected_cb);

/**
 * @brief Parse raw WireGuard .conf file text and save to NVS.
 *
 * @param conf_text INI-format WireGuard configuration string.
 * @return ESP_OK on successful parse & save.
 */
esp_err_t wireguard_manager_set_config_from_text(const char *conf_text);

/**
 * @brief Save structured configuration to NVS.
 */
esp_err_t wireguard_manager_set_config(const wg_manager_config_t *config);

/**
 * @brief Get current configuration.
 */
esp_err_t wireguard_manager_get_config(wg_manager_config_t *out_config);

/**
 * @brief Get runtime tunnel status and telemetry.
 */
void wireguard_manager_get_status(wireguard_status_t *out_status);

/**
 * @brief Enable or disable WireGuard connection.
 */
esp_err_t wireguard_manager_set_enabled(bool enabled);

/**
 * @brief Called by Wi-Fi manager when Station connects to trigger SNTP and WireGuard.
 */
void wireguard_manager_on_wifi_connected(void);

/**
 * @brief Called by Wi-Fi manager when Station disconnects.
 */
void wireguard_manager_on_wifi_disconnected(void);

#ifdef __cplusplus
}
#endif
