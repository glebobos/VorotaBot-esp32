#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_MGR";
static const char *NVS_NAMESPACE = "config";

esp_err_t nvs_manager_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or format outdated. Erasing and retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS flash storage initialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvs_manager_get_str(const char *key, char *out_val, size_t max_len, const char *default_val) {
    if (!key || !out_val || max_len == 0) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (default_val) {
            strncpy(out_val, default_val, max_len - 1);
            out_val[max_len - 1] = '\0';
        }
        return err;
    }
    
    size_t req_len = max_len;
    err = nvs_get_str(handle, key, out_val, &req_len);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        if (default_val) {
            strncpy(out_val, default_val, max_len - 1);
            out_val[max_len - 1] = '\0';
        }
    }
    return err;
}

esp_err_t nvs_manager_set_str(const char *key, const char *val) {
    if (!key || !val) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(handle, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_i32(const char *key, int32_t *out_val, int32_t default_val) {
    if (!key || !out_val) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        *out_val = default_val;
        return err;
    }
    
    err = nvs_get_i32(handle, key, out_val);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        *out_val = default_val;
    }
    return err;
}

esp_err_t nvs_manager_set_i32(const char *key, int32_t val) {
    if (!key) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_i32(handle, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_get_blob(const char *key, void *out_buf, size_t *length) {
    if (!key || !out_buf || !length) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_get_blob(handle, key, out_buf, length);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_set_blob(const char *key, const void *data, size_t length) {
    if (!key || !data || length == 0) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_blob(handle, key, data, length);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_erase_key(const char *key) {
    if (!key) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t nvs_manager_factory_reset(void) {
    ESP_LOGW(TAG, "Executing factory reset: erasing '%s' namespace...", NVS_NAMESPACE);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
