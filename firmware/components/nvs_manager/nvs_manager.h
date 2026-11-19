#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Non-Volatile Storage (NVS) with automatic recovery on truncation.
 *
 * @return ESP_OK on success.
 */
esp_err_t nvs_manager_init(void);

/**
 * @brief Read a string value from NVS.
 *
 * @param key Key name (max 15 characters).
 * @param out_val Buffer to receive null-terminated string.
 * @param max_len Maximum length of out_val buffer.
 * @param default_val Fallback string if key not found.
 * @return ESP_OK if found, ESP_ERR_NVS_NOT_FOUND if default used.
 */
esp_err_t nvs_manager_get_str(const char *key, char *out_val, size_t max_len, const char *default_val);

/**
 * @brief Write a string value to NVS and commit.
 *
 * @param key Key name.
 * @param val String value to save.
 * @return ESP_OK on success.
 */
esp_err_t nvs_manager_set_str(const char *key, const char *val);

/**
 * @brief Read an int32 value from NVS.
 *
 * @param key Key name.
 * @param out_val Pointer to int32 to receive value.
 * @param default_val Fallback value if key not found.
 * @return ESP_OK if found, ESP_ERR_NVS_NOT_FOUND if default used.
 */
esp_err_t nvs_manager_get_i32(const char *key, int32_t *out_val, int32_t default_val);

/**
 * @brief Write an int32 value to NVS and commit.
 *
 * @param key Key name.
 * @param val Integer value to save.
 * @return ESP_OK on success.
 */
esp_err_t nvs_manager_set_i32(const char *key, int32_t val);

/**
 * @brief Read binary blob from NVS.
 */
esp_err_t nvs_manager_get_blob(const char *key, void *out_buf, size_t *length);

/**
 * @brief Write binary blob to NVS and commit.
 */
esp_err_t nvs_manager_set_blob(const char *key, const void *data, size_t length);

/**
 * @brief Erase a specific key from NVS.
 */
esp_err_t nvs_manager_erase_key(const char *key);

/**
 * @brief Erase entire configuration namespace (Factory Reset).
 */
esp_err_t nvs_manager_factory_reset(void);

#ifdef __cplusplus
}
#endif
