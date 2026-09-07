#include "aws_route53.h"
#include "nvs_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"

static const char *TAG = "ROUTE53";

static aws_route53_config_t s_config;
static aws_route53_status_t s_status;
static SemaphoreHandle_t s_lock = NULL;

static void hex_encode(const unsigned char *bin, size_t bin_len, char *out_hex) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < bin_len; i++) {
        out_hex[i * 2]     = hex_chars[(bin[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex_chars[bin[i] & 0x0F];
    }
    out_hex[bin_len * 2] = '\0';
}

static void sha256_hex(const char *data, size_t len, char *out_hex) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)data, len, hash, 0);
    hex_encode(hash, sizeof(hash), out_hex);
}

static void hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len, unsigned char *out_bin) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md_info, (const unsigned char *)key, key_len, (const unsigned char *)data, data_len, out_bin);
}

static esp_err_t load_config_from_nvs(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_config, 0, sizeof(s_config));

    nvs_manager_get_str("aws_key", s_config.access_key_id, sizeof(s_config.access_key_id), "");
    nvs_manager_get_str("aws_sec", s_config.secret_access_key, sizeof(s_config.secret_access_key), "");
    nvs_manager_get_str("aws_dom", s_config.root_domain, sizeof(s_config.root_domain), "glebos.click");
    nvs_manager_get_str("aws_zone", s_config.hosted_zone_id, sizeof(s_config.hosted_zone_id), "");
    nvs_manager_get_str("aws_host", s_config.record_hostname, sizeof(s_config.record_hostname), "vorota.glebos.click");

    if (strlen(s_config.record_hostname) == 0 && strlen(s_config.root_domain) > 0) {
        snprintf(s_config.record_hostname, sizeof(s_config.record_hostname), "vorota.%.120s", s_config.root_domain);
    }

    s_status.is_configured = (strlen(s_config.access_key_id) > 0 &&
                              strlen(s_config.secret_access_key) > 0 &&
                              strlen(s_config.hosted_zone_id) > 0 &&
                              strlen(s_config.record_hostname) > 0);
    snprintf(s_status.fqdn, sizeof(s_status.fqdn), "%s", s_config.record_hostname);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void execute_route53_update(const char *ip) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    if (tm_utc.tm_year < (2024 - 1900)) {
        ESP_LOGE(TAG, "Cannot update Route 53: Clock not synchronized yet");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", "Clock not synced");
        xSemaphoreGive(s_lock);
        return;
    }

    char amz_date[20];
    char date_stamp[10];
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &tm_utc);

    char hostname_with_dot[164];
    snprintf(hostname_with_dot, sizeof(hostname_with_dot), "%s.", s_config.record_hostname);

    char *xml_payload = malloc(1024);
    char *canonical_req = malloc(1024);
    if (!xml_payload || !canonical_req) {
        ESP_LOGE(TAG, "Failed to allocate memory for Route 53 payload");
        free(xml_payload);
        free(canonical_req);
        return;
    }

    // 1. Build XML Change Batch Payload
    snprintf(xml_payload, 1024,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<ChangeResourceRecordSetsRequest xmlns=\"https://route53.amazonaws.com/doc/2013-04-01/\">"
          "<ChangeBatch>"
            "<Changes>"
              "<Change>"
                "<Action>UPSERT</Action>"
                "<ResourceRecordSet>"
                  "<Name>%s</Name>"
                  "<Type>A</Type>"
                  "<TTL>60</TTL>"
                  "<ResourceRecords>"
                    "<ResourceRecord><Value>%s</Value></ResourceRecord>"
                  "</ResourceRecords>"
                "</ResourceRecordSet>"
              "</Change>"
            "</Changes>"
          "</ChangeBatch>"
        "</ChangeResourceRecordSetsRequest>",
        hostname_with_dot, ip);

    // 2. Hash Payload
    char payload_hash[65];
    sha256_hex(xml_payload, strlen(xml_payload), payload_hash);

    // 3. Create Canonical Request
    char canonical_uri[128];
    snprintf(canonical_uri, sizeof(canonical_uri), "/2013-04-01/hostedzone/%s/rrset", s_config.hosted_zone_id);

    snprintf(canonical_req, 1024,
        "POST\n"
        "%s\n"
        "\n"
        "content-type:application/xml\n"
        "host:route53.amazonaws.com\n"
        "x-amz-date:%s\n"
        "\n"
        "content-type;host;x-amz-date\n"
        "%s",
        canonical_uri, amz_date, payload_hash);

    char canonical_req_hash[65];
    sha256_hex(canonical_req, strlen(canonical_req), canonical_req_hash);

    // 4. Create StringToSign
    char credential_scope[64];
    snprintf(credential_scope, sizeof(credential_scope), "%s/us-east-1/route53/aws4_request", date_stamp);

    char string_to_sign[256];
    snprintf(string_to_sign, sizeof(string_to_sign),
        "AWS4-HMAC-SHA256\n"
        "%s\n"
        "%s\n"
        "%s",
        amz_date, credential_scope, canonical_req_hash);

    // 5. Derive Signing Key
    char k_secret[132];
    snprintf(k_secret, sizeof(k_secret), "AWS4%s", s_config.secret_access_key);

    unsigned char k_date[32];
    hmac_sha256(k_secret, strlen(k_secret), date_stamp, strlen(date_stamp), k_date);

    unsigned char k_region[32];
    hmac_sha256(k_date, sizeof(k_date), "us-east-1", 9, k_region);

    unsigned char k_service[32];
    hmac_sha256(k_region, sizeof(k_region), "route53", 7, k_service);

    unsigned char k_signing[32];
    hmac_sha256(k_service, sizeof(k_service), "aws4_request", 12, k_signing);

    unsigned char signature_bin[32];
    hmac_sha256(k_signing, sizeof(k_signing), string_to_sign, strlen(string_to_sign), signature_bin);

    char signature_hex[65];
    hex_encode(signature_bin, sizeof(signature_bin), signature_hex);

    // 6. Build Authorization Header
    char auth_header[384];
    snprintf(auth_header, sizeof(auth_header),
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=content-type;host;x-amz-date, Signature=%s",
        s_config.access_key_id, credential_scope, signature_hex);

    // 7. Perform HTTPS Request
    char url[256];
    snprintf(url, sizeof(url), "https://route53.amazonaws.com%s", canonical_uri);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for Route 53");
        free(xml_payload);
        free(canonical_req);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/xml");
    esp_http_client_set_header(client, "Host", "route53.amazonaws.com");
    esp_http_client_set_header(client, "x-amz-date", amz_date);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, xml_payload, strlen(xml_payload));

    ESP_LOGI(TAG, "Submitting DNS update to Route 53 for %s -> %s...", s_config.record_hostname, ip);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK && (status_code == 200 || status_code == 201)) {
        ESP_LOGI(TAG, "Route 53 DNS record successfully updated! (HTTP %d)", status_code);
        s_status.is_synced = true;
        s_status.last_sync_epoch = (uint32_t)now;
        snprintf(s_status.registered_ip, sizeof(s_status.registered_ip), "%s", ip);
        s_status.last_error[0] = '\0';
    } else {
        ESP_LOGE(TAG, "Route 53 update failed: err=%s, HTTP status=%d", esp_err_to_name(err), status_code);
        s_status.is_synced = false;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "HTTP %d (%s)", status_code, esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);

    esp_http_client_cleanup(client);
    free(xml_payload);
    free(canonical_req);
}

typedef struct {
    char target_ip[32];
} sync_task_arg_t;

static void sync_worker_task(void *pvParameters) {
    sync_task_arg_t *arg = (sync_task_arg_t *)pvParameters;
    if (arg) {
        execute_route53_update(arg->target_ip);
        free(arg);
    }
    vTaskDelete(NULL);
}

esp_err_t aws_route53_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return load_config_from_nvs();
}

esp_err_t aws_route53_set_config(const aws_route53_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_config, config, sizeof(aws_route53_config_t));

    nvs_manager_set_str("aws_key", s_config.access_key_id);
    nvs_manager_set_str("aws_sec", s_config.secret_access_key);
    nvs_manager_set_str("aws_dom", s_config.root_domain);
    nvs_manager_set_str("aws_zone", s_config.hosted_zone_id);
    nvs_manager_set_str("aws_host", s_config.record_hostname);

    s_status.is_configured = (strlen(s_config.access_key_id) > 0 &&
                              strlen(s_config.secret_access_key) > 0 &&
                              strlen(s_config.hosted_zone_id) > 0 &&
                              strlen(s_config.record_hostname) > 0);
    snprintf(s_status.fqdn, sizeof(s_status.fqdn), "%s", s_config.record_hostname);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "AWS Route 53 configuration saved to NVS");
    return ESP_OK;
}

esp_err_t aws_route53_get_config(aws_route53_config_t *out_config) {
    if (!out_config) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_config, &s_config, sizeof(aws_route53_config_t));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void aws_route53_get_status(aws_route53_status_t *out_status) {
    if (!out_status) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_status, &s_status, sizeof(aws_route53_status_t));
    xSemaphoreGive(s_lock);
}

esp_err_t aws_route53_sync_record(const char *ip_to_register) {
    if (!ip_to_register || strlen(ip_to_register) == 0) return ESP_ERR_INVALID_ARG;

    if (!s_status.is_configured) {
        ESP_LOGW(TAG, "Cannot sync Route 53: Not configured");
        return ESP_ERR_NOT_SUPPORTED;
    }

    sync_task_arg_t *arg = malloc(sizeof(sync_task_arg_t));
    if (!arg) return ESP_ERR_NO_MEM;

    snprintf(arg->target_ip, sizeof(arg->target_ip), "%s", ip_to_register);

    if (xTaskCreate(sync_worker_task, "r53_sync", 16384, arg, 5, NULL) != pdPASS) {
        free(arg);
        return ESP_FAIL;
    }

    return ESP_OK;
}
