#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the Captive Portal DNS server task (UDP port 53).
 * Resolves configured domains and standard captive detection probes to the AP IP.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t dns_server_start(void);

/**
 * @brief Stop the Captive Portal DNS server and free socket resources.
 */
void dns_server_stop(void);

/**
 * @brief Check if the DNS server task is currently running.
 *
 * @return true if running, false otherwise.
 */
bool dns_server_is_running(void);

#ifdef __cplusplus
}
#endif
