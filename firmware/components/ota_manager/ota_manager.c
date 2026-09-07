#include "ota_manager.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "OTA_MGR";

static const esp_partition_t *s_update_partition = NULL;
static esp_ota_handle_t s_update_handle = 0;
static ota_status_t s_status = {0};

esp_err_t ota_manager_init(void) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    snprintf(s_status.current_app_version, sizeof(s_status.current_app_version), "%s", app_desc ? app_desc->version : "1.0.0");
    if (running) {
        snprintf(s_status.current_partition_label, sizeof(s_status.current_partition_label), "%s", running->label);
    }
    if (next) {
        snprintf(s_status.next_partition_label, sizeof(s_status.next_partition_label), "%s", next->label);
    }

    ESP_LOGI(TAG, "OTA Subsystem Ready: Running='%s' (Ver: %s), Next Target='%s'",
             s_status.current_partition_label, s_status.current_app_version, s_status.next_partition_label);
    return ESP_OK;
}

esp_err_t ota_manager_begin(size_t image_size) {
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_update_partition) {
        ESP_LOGE(TAG, "Failed to find available OTA update partition");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing OTA update to partition '%s' at offset 0x%08" PRIx32 " (Size: %u bytes)",
             s_update_partition->label, (uint32_t)s_update_partition->address, (unsigned int)image_size);

    esp_err_t err = esp_ota_begin(s_update_partition, image_size > 0 ? image_size : OTA_WITH_SEQUENTIAL_WRITES, &s_update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_update_handle = 0;
        return err;
    }

    s_status.ota_size_bytes = image_size;
    s_status.written_bytes = 0;
    s_status.is_in_progress = true;
    return ESP_OK;
}

esp_err_t ota_manager_write(const void *data, size_t length) {
    if (!s_status.is_in_progress || s_update_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_write(s_update_handle, data, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }

    s_status.written_bytes += length;
    return ESP_OK;
}

esp_err_t ota_manager_end(void) {
    if (!s_status.is_in_progress || s_update_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(s_update_handle);
    s_update_handle = 0;
    s_status.is_in_progress = false;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end validation failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update successfully flashed and verified! New boot partition: '%s'", s_update_partition->label);
    return ESP_OK;
}

void ota_manager_abort(void) {
    if (s_update_handle != 0) {
        esp_ota_abort(s_update_handle);
        s_update_handle = 0;
    }
    s_status.is_in_progress = false;
    ESP_LOGW(TAG, "OTA update session aborted");
}

void ota_manager_get_status(ota_status_t *out_status) {
    if (out_status) {
        memcpy(out_status, &s_status, sizeof(ota_status_t));
    }
}

static void reboot_timer_callback(void* arg) {
    ESP_LOGI(TAG, "Rebooting system to apply firmware...");
    esp_restart();
}

void ota_manager_reboot_delayed(uint32_t delay_ms) {
    const esp_timer_create_args_t timer_args = {
        .callback = &reboot_timer_callback,
        .name = "ota_reboot"
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
        ESP_LOGI(TAG, "System reboot scheduled in %lu ms", (unsigned long)delay_ms);
    } else {
        esp_restart();
    }
}
