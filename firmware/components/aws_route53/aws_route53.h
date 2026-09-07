#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char access_key_id[64];
    char secret_access_key[128];
    char root_domain[128];
    char hosted_zone_id[64];
    char record_hostname[160]; // e.g. "vorota.example.com"
} aws_route53_config_t;

typedef struct {
    bool is_configured;
    bool is_synced;
    char registered_ip[32];
    char fqdn[160];
    uint32_t last_sync_epoch;
    char last_error[64];
} aws_route53_status_t;

/**
 * @brief Initialize AWS Route 53 manager and load configuration from NVS.
 */
esp_err_t aws_route53_init(void);

/**
 * @brief Save AWS configuration to NVS.
 */
esp_err_t aws_route53_set_config(const aws_route53_config_t *config);

/**
 * @brief Get current AWS Route 53 configuration.
 */
esp_err_t aws_route53_get_config(aws_route53_config_t *out_config);

/**
 * @brief Get runtime synchronization status and telemetry.
 */
void aws_route53_get_status(aws_route53_status_t *out_status);

/**
 * @brief Trigger asynchronous DNS record registration / upsert for the given IP address.
 *
 * @param ip_to_register Target IPv4 address (typically WireGuard IP).
 * @return ESP_OK if update task was queued.
 */
esp_err_t aws_route53_sync_record(const char *ip_to_register);

#ifdef __cplusplus
}
#endif
