#include "web_server.h"
#include "wifi_manager.h"
#include "nvs_manager.h"
#include "ota_manager.h"
#include "gate_controller.h"
#include "wireguard_manager.h"
#include "aws_route53.h"
#include "ble_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WEB_SERVER";

static httpd_handle_t s_server = NULL;

/* Content-Type mapping */
static const char* get_content_type(const char *path) {
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".css")) return "text/css";
    if (strstr(path, ".js") || strstr(path, ".mjs")) return "application/javascript";
    if (strstr(path, ".json")) return "application/json";
    if (strstr(path, ".svg")) return "image/svg+xml";
    if (strstr(path, ".png")) return "image/png";
    if (strstr(path, ".jpg") || strstr(path, ".jpeg")) return "image/jpeg";
    if (strstr(path, ".ico")) return "image/x-icon";
    if (strstr(path, ".woff2")) return "font/woff2";
    if (strstr(path, ".wasm")) return "application/wasm";
    return "text/plain";
}

/* Captive Portal redirect helper */
static esp_err_t captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Check for standard captive detection probe URLs */
static bool is_captive_probe(const char *uri) {
    return (strcmp(uri, "/generate_204") == 0 ||
            strcmp(uri, "/gen_204") == 0 ||
            strcmp(uri, "/canonical.html") == 0 ||
            strcmp(uri, "/connecttest.txt") == 0 ||
            strcmp(uri, "/hotspot-detect.html") == 0 ||
            strcmp(uri, "/ncsi.txt") == 0 ||
            strcmp(uri, "/redirect") == 0);
}

/* SPIFFS File Server with transparent GZIP support */
static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *filepath) {
    char gz_filepath[160];
    snprintf(gz_filepath, sizeof(gz_filepath), "%s.gz", filepath);

    struct stat st;
    bool is_gz = false;
    const char *final_path = filepath;

    if (stat(gz_filepath, &st) == 0) {
        is_gz = true;
        final_path = gz_filepath;
    } else if (stat(filepath, &st) != 0) {
        // Fallback to index.html for SPA client routes
        if (strstr(filepath, "/assets/") == NULL && strcmp(filepath, "/spiffs/favicon.ico") != 0) {
            return serve_spiffs_file(req, "/spiffs/index.html");
        }
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    FILE *fd = fopen(final_path, "rb");
    if (!fd) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));
    if (is_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Cache headers
    if (strstr(filepath, "/assets/")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    } else if (strstr(filepath, "index.html")) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    }

    char chunk[1024];
    size_t bytes_read;
    do {
        bytes_read = fread(chunk, 1, sizeof(chunk), fd);
        if (bytes_read > 0) {
            if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
                fclose(fd);
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_FAIL;
            }
        }
    } while (bytes_read > 0);

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Static assets & Catch-all handler */
static esp_err_t spiffs_catch_all_handler(httpd_req_t *req) {
    if (is_captive_probe(req->uri)) {
        return captive_redirect(req);
    }

    if (strcmp(req->uri, "/favicon.ico") == 0) {
        return serve_spiffs_file(req, "/spiffs/favicon.ico");
    }

    char filepath[160];
    const char *quest = strchr(req->uri, '?');
    size_t path_len = quest ? (size_t)(quest - req->uri) : strlen(req->uri);
    if (path_len >= sizeof(filepath) - 8) {
        path_len = sizeof(filepath) - 9;
    }

    if (strcmp(req->uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%.*s", (int)path_len, req->uri);
    }

    return serve_spiffs_file(req, filepath);
}

/* Helper to read body buffer */
static char* read_request_body(httpd_req_t *req) {
    if (req->content_len <= 0) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        return NULL;
    }
    buf[received] = '\0';
    return buf;
}

/* =========================================================================
 * REST API: Gate Control
 * ========================================================================= */

/* POST /api/gate/garage */
static esp_err_t api_gate_garage_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    garage_action_t action = GARAGE_ACTION_FULL;

    if (body) {
        if (strstr(body, "\"action\":\"vent\"") || strstr(body, "\"action\": \"vent\"")) {
            action = GARAGE_ACTION_VENT;
        } else if (strstr(body, "\"action\":\"full\"") || strstr(body, "\"action\": \"full\"")) {
            action = GARAGE_ACTION_FULL;
        } else if (strstr(body, "\"action\"")) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid garage action. Use 'full' or 'vent'\"}");
            return ESP_OK;
        }
        free(body);
    }

    esp_err_t err = gate_garage_trigger(action);
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_OK) {
        if (action == GARAGE_ACTION_VENT) {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"garage_vent\"}");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"garage_full\"}");
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Interlock safety delay active\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to trigger garage gate\"}");
    }
    return ESP_OK;
}



/* POST /api/gate/driveway */
static esp_err_t api_gate_driveway_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    driveway_action_t action = DRIVEWAY_ACTION_OPEN;

    if (body) {
        if (strstr(body, "\"action\":\"close\"") || strstr(body, "\"action\": \"close\"")) {
            action = DRIVEWAY_ACTION_CLOSE;
        } else if (strstr(body, "\"action\":\"open\"") || strstr(body, "\"action\": \"open\"")) {
            action = DRIVEWAY_ACTION_OPEN;
        } else if (strstr(body, "\"action\"")) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid driveway action. Use 'open' or 'close'\"}");
            return ESP_OK;
        }
        free(body);
    }

    esp_err_t err = gate_driveway_trigger(action);
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_OK) {
        if (action == DRIVEWAY_ACTION_CLOSE) {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"driveway_close\"}");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"driveway_open\"}");
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Interlock safety delay active\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to trigger driveway gate\"}");
    }
    return ESP_OK;
}



/* GET /api/gate/status */
static esp_err_t api_gate_status_handler(httpd_req_t *req) {
    gate_status_t st;
    gate_controller_get_status(&st);

    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"garage_state\":%d,"
        "\"garage_state_str\":\"%s\","
        "\"driveway_state\":%d,"
        "\"driveway_state_str\":\"%s\","
        "\"pulse_duration_ms\":%lu,"
        "\"interlock_delay_ms\":%lu,"
        "\"last_garage_ts\":%llu,"
        "\"last_driveway_ts\":%llu,"
        "\"last_action\":\"%s\""
        "}",
        st.garage_state, gate_state_to_str(st.garage_state),
        st.driveway_state, gate_state_to_str(st.driveway_state),
        (unsigned long)st.pulse_duration_ms, (unsigned long)st.interlock_delay_ms,
        (unsigned long long)st.last_garage_action_ts, (unsigned long long)st.last_driveway_action_ts,
        st.last_action_desc
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* POST /api/gate/settings */
static esp_err_t api_gate_settings_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required");
        return ESP_FAIL;
    }

    char *pulse_ptr = strstr(body, "\"pulse_ms\":");
    if (pulse_ptr) {
        uint32_t ms = (uint32_t)atoi(pulse_ptr + 11);
        if (ms >= 100 && ms <= 2000) {
            gate_controller_set_pulse_duration(ms);
        }
    }

    char *interlock_ptr = strstr(body, "\"interlock_ms\":");
    if (interlock_ptr) {
        uint32_t ms = (uint32_t)atoi(interlock_ptr + 15);
        if (ms >= 500 && ms <= 5000) {
            gate_controller_set_interlock_delay(ms);
        }
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Gate timings updated\"}");
    return ESP_OK;
}

/* =========================================================================
 * REST API: WireGuard VPN
 * ========================================================================= */



/* POST /api/wireguard/toggle */
static esp_err_t api_wireguard_toggle_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    bool enable = true;
    if (body) {
        if (strstr(body, "\"enabled\":false") || strstr(body, "\"enabled\": false") || strstr(body, "false")) {
            enable = false;
        }
        free(body);
    }

    wireguard_manager_set_enabled(enable);
    ble_service_set_vpn_state(enable);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, enable ? "{\"status\":\"ok\",\"vpn\":\"enabled\"}" : "{\"status\":\"ok\",\"vpn\":\"disabled\"}");
    return ESP_OK;
}

/* POST /api/wireguard/config */
static esp_err_t api_wireguard_config_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configuration body required");
        return ESP_FAIL;
    }

    esp_err_t err = wireguard_manager_set_config_from_text(body);
    free(body);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"WireGuard configuration updated\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid WireGuard configuration format");
    }
    return ESP_OK;
}

/* =========================================================================
 * REST API: AWS Route 53
 * ========================================================================= */



/* POST /api/route53/sync */
static esp_err_t api_route53_sync_handler(httpd_req_t *req) {
    wireguard_status_t wg_st;
    wireguard_manager_get_status(&wg_st);

    const char *target_ip = wg_st.is_connected ? wg_st.assigned_ip : wifi_manager_get_sta_ip();
    esp_err_t err = aws_route53_sync_record(target_ip);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Route 53 DNS sync initiated\"}");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Route 53 not configured or missing IP\"}");
    }
    return ESP_OK;
}

/* =========================================================================
 * REST API: Wi-Fi Management
 * ========================================================================= */

/* POST /api/wifi/connect */
static esp_err_t api_wifi_connect_handler(httpd_req_t *req) {
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing credentials");
        return ESP_FAIL;
    }

    char ssid[33] = {0};
    char pass[65] = {0};

    char *s = strstr(body, "\"ssid\":\"");
    if (s) {
        char *end = strchr(s + 8, '\"');
        if (end) {
            size_t len = MIN((size_t)(end - (s + 8)), sizeof(ssid) - 1);
            strncpy(ssid, s + 8, len);
        }
    }

    char *p = strstr(body, "\"password\":\"");
    if (p) {
        char *end = strchr(p + 12, '\"');
        if (end) {
            size_t len = MIN((size_t)(end - (p + 12)), sizeof(pass) - 1);
            strncpy(pass, p + 12, len);
        }
    }
    free(body);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_manager_set_sta_credentials(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Connecting to Wi-Fi...\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initiate connection");
    }
    return ESP_OK;
}

/* =========================================================================
 * REST API: System & OTA
 * ========================================================================= */

/* GET /api/system/info */
static esp_err_t api_system_info_handler(httpd_req_t *req) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ota_status_t ota_st;
    ota_manager_get_status(&ota_st);

    wifi_mgr_config_t wifi_cfg;
    wifi_manager_get_config(&wifi_cfg);

    gate_status_t gate_st;
    gate_controller_get_status(&gate_st);

    wireguard_status_t wg_st;
    wireguard_manager_get_status(&wg_st);

    aws_route53_status_t r53_st;
    aws_route53_get_status(&r53_st);

    char json[1024];
    snprintf(json, sizeof(json),
        "{"
        "\"app\":\"VorotaBot-esp32\","
        "\"version\":\"%s\","
        "\"chip\":\"ESP32-C3\","
        "\"cores\":%d,"
        "\"cpu_freq_mhz\":%lu,"
        "\"heap_free\":%lu,"
        "\"uptime_s\":%lld,"
        "\"wifi\":{"
          "\"mode\":\"%s\","
          "\"sta_connected\":%s,"
          "\"sta_ssid\":\"%s\","
          "\"sta_ip\":\"%s\","
          "\"sta_rssi\":%d"
        "},"
        "\"wireguard\":{"
          "\"configured\":%s,"
          "\"enabled\":%s,"
          "\"connected\":%s,"
          "\"ip\":\"%s\","
          "\"endpoint\":\"%s\""
        "},"
        "\"route53\":{"
          "\"configured\":%s,"
          "\"synced\":%s,"
          "\"fqdn\":\"%s\""
        "},"
        "\"gates\":{"
          "\"garage_state\":%d,"
          "\"driveway_state\":%d,"
          "\"last_action\":\"%s\""
        "},"
        "\"ota_partition\":\"%s\","
        "\"build_time\":\"%s %s\""
        "}",
        ota_st.current_app_version,
        chip_info.cores,
        (unsigned long)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (unsigned long)esp_get_free_heap_size(),
        (long long)(esp_timer_get_time() / 1000000),
        wifi_cfg.sta_enabled ? "AP+STA" : "AP_ONLY",
        wifi_cfg.sta_connected ? "true" : "false",
        wifi_cfg.sta_ssid,
        wifi_cfg.sta_ip,
        wifi_manager_get_sta_rssi(),
        wg_st.is_configured ? "true" : "false",
        wg_st.is_enabled ? "true" : "false",
        wg_st.is_connected ? "true" : "false",
        wg_st.assigned_ip,
        wg_st.endpoint,
        r53_st.is_configured ? "true" : "false",
        r53_st.is_synced ? "true" : "false",
        r53_st.fqdn,
        gate_st.garage_state,
        gate_st.driveway_state,
        gate_st.last_action_desc,
        ota_st.current_partition_label,
        __DATE__, __TIME__
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* POST /api/system/restart */
static esp_err_t api_system_restart_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Rebooting ESP32...\"}");
    ota_manager_reboot_delayed(800);
    return ESP_OK;
}

/* POST /api/system/factory-reset */
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req) {
    nvs_manager_factory_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Factory reset complete. Rebooting...\"}");
    ota_manager_reboot_delayed(800);
    return ESP_OK;
}

/* POST /api/system/ota */
static esp_err_t api_system_ota_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Starting OTA stream upload (Total size: %d bytes)...", req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length header required");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_begin(req->content_len);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initialize OTA partition");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA receive timeout / error");
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload transfer failed");
            return ESP_FAIL;
        }

        err = ota_manager_write(buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA flash write error");
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }

        remaining -= recv_len;
    }

    err = ota_manager_end();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware verification failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Update complete! Rebooting in 1.5s...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA Flash successful! Device rebooting...\"}");

    ota_manager_reboot_delayed(1500);
    return ESP_OK;
}


/* SPIFFS Mounting */
static esp_err_t mount_spiffs(void) {
    ESP_LOGI(TAG, "Mounting SPIFFS filesystem on /spiffs ('storage' partition)...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Partition mounted: Total=%d KB, Used=%d KB, Free=%d KB",
                 (int)(total / 1024), (int)(used / 1024), (int)((total - used) / 1024));
    }
    return ESP_OK;
}

esp_err_t web_server_start(const web_server_config_t *user_config) {
    mount_spiffs();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = user_config ? user_config->max_open_sockets : 8;
    config.max_uri_handlers = 32;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    ESP_LOGI(TAG, "Starting VorotaBot HTTP Daemon on port %d...", config.server_port);
    if (httpd_start(&s_server, &config) == ESP_OK) {
        // Gate Routes
        httpd_uri_t uri_garage = { .uri = "/api/gate/garage", .method = HTTP_POST, .handler = api_gate_garage_handler };
        httpd_register_uri_handler(s_server, &uri_garage);

        httpd_uri_t uri_driveway = { .uri = "/api/gate/driveway", .method = HTTP_POST, .handler = api_gate_driveway_handler };
        httpd_register_uri_handler(s_server, &uri_driveway);

        httpd_uri_t uri_gate_st = { .uri = "/api/gate/status", .method = HTTP_GET, .handler = api_gate_status_handler };
        httpd_register_uri_handler(s_server, &uri_gate_st);

        httpd_uri_t uri_gate_settings = { .uri = "/api/gate/settings", .method = HTTP_POST, .handler = api_gate_settings_handler };
        httpd_register_uri_handler(s_server, &uri_gate_settings);

        // WireGuard Routes
        httpd_uri_t uri_wg_toggle = { .uri = "/api/wireguard/toggle", .method = HTTP_POST, .handler = api_wireguard_toggle_handler };
        httpd_register_uri_handler(s_server, &uri_wg_toggle);

        httpd_uri_t uri_wg_cfg = { .uri = "/api/wireguard/config", .method = HTTP_POST, .handler = api_wireguard_config_handler };
        httpd_register_uri_handler(s_server, &uri_wg_cfg);

        // Route 53 Routes
        httpd_uri_t uri_r53_sync = { .uri = "/api/route53/sync", .method = HTTP_POST, .handler = api_route53_sync_handler };
        httpd_register_uri_handler(s_server, &uri_r53_sync);

        // Wi-Fi Connection Route
        httpd_uri_t uri_wifi_conn = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler };
        httpd_register_uri_handler(s_server, &uri_wifi_conn);

        // System Routes
        httpd_uri_t uri_info = { .uri = "/api/system/info", .method = HTTP_GET, .handler = api_system_info_handler };
        httpd_register_uri_handler(s_server, &uri_info);

        httpd_uri_t uri_restart = { .uri = "/api/system/restart", .method = HTTP_POST, .handler = api_system_restart_handler };
        httpd_register_uri_handler(s_server, &uri_restart);

        httpd_uri_t uri_fact_reset = { .uri = "/api/system/factory-reset", .method = HTTP_POST, .handler = api_system_factory_reset_handler };
        httpd_register_uri_handler(s_server, &uri_fact_reset);

        httpd_uri_t uri_ota = { .uri = "/api/system/ota", .method = HTTP_POST, .handler = api_system_ota_handler };
        httpd_register_uri_handler(s_server, &uri_ota);

        // Catch-all SPIFFS web assets handler
        httpd_uri_t uri_spiffs = { .uri = "/*", .method = HTTP_GET, .handler = spiffs_catch_all_handler };
        httpd_register_uri_handler(s_server, &uri_spiffs);

        ESP_LOGI(TAG, "VorotaBot HTTP server started successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start HTTP Daemon!");
    return ESP_FAIL;
}

void web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    esp_vfs_spiffs_unregister("storage");
}

httpd_handle_t web_server_get_handle(void) {
    return s_server;
}
