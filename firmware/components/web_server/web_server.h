#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;
    uint8_t max_open_sockets;
} web_server_config_t;

/**
 * @brief Initialize SPIFFS filesystem and start HTTP server.
 *
 * @param config Optional server configuration (or NULL for standard defaults).
 * @return ESP_OK on success.
 */
esp_err_t web_server_start(const web_server_config_t *config);

/**
 * @brief Stop the HTTP server and unmount SPIFFS filesystem.
 */
void web_server_stop(void);

/**
 * @brief Get underlying httpd handle.
 */
httpd_handle_t web_server_get_handle(void);

#ifdef __cplusplus
}
#endif
