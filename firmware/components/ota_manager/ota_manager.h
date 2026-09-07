#pragma once

#include "esp_err.h"
#include "esp_ota_ops.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char current_app_version[32];
    char current_partition_label[32];
    char next_partition_label[32];
    size_t ota_size_bytes;
    size_t written_bytes;
    bool is_in_progress;
} ota_status_t;

/**
 * @brief Initialize OTA manager and query partition layout.
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Begin an OTA update session.
 * Prepares the next available OTA app partition for writing.
 *
 * @param image_size Expected total binary image size in bytes (or 0 if unknown).
 * @return ESP_OK on success.
 */
esp_err_t ota_manager_begin(size_t image_size);

/**
 * @brief Write binary chunk received over HTTP POST / OTA stream.
 *
 * @param data Chunk buffer.
 * @param length Chunk length.
 * @return ESP_OK on success.
 */
esp_err_t ota_manager_write(const void *data, size_t length);

/**
 * @brief Finalize OTA write, validate image header and checksum, and set boot partition.
 *
 * @return ESP_OK on success.
 */
esp_err_t ota_manager_end(void);

/**
 * @brief Abort currently running OTA session and release partition handle.
 */
void ota_manager_abort(void);

/**
 * @brief Get current OTA status information.
 */
void ota_manager_get_status(ota_status_t *out_status);

/**
 * @brief Schedule a delayed system reboot (e.g. 1000ms) after successful OTA flashing.
 */
void ota_manager_reboot_delayed(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
