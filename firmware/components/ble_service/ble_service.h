#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_vpn_toggle_cb_t)(bool enable);

/**
 * @brief Initialize NimBLE Bluetooth stack and advertise VorotaBot Provisioning Service.
 *
 * @param vpn_cb Callback invoked when VPN state is toggled via BLE.
 * @return ESP_OK on success.
 */
esp_err_t ble_service_init(ble_vpn_toggle_cb_t vpn_cb);

/**
 * @brief Notify connected BLE clients of updated Wi-Fi or VPN status.
 */
void ble_service_notify_status(const char *status_str);

/**
 * @brief Update internal BLE state for VPN enabled/disabled.
 */
void ble_service_set_vpn_state(bool enabled);

/**
 * @brief Check if a BLE client is actively connected.
 */
bool ble_service_is_connected(void);

#ifdef __cplusplus
}
#endif
