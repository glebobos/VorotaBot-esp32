#include "web_server.h"
#include "http_util.h"
#include "auth.h"
#include "wifi_manager.h"
#include "nvs_manager.h"
#include "ota_manager.h"
#include "gate_controller.h"
#include "wireguard_manager.h"
#include "aws_route53.h"
#include "dns_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_spiffs.h"
#include "esp_random.h"
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

/* Embedded Web Assets */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static esp_err_t serve_embedded_index_html(httpd_req_t *req) {
    size_t len = index_html_gz_end - index_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start, len);
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
    } else if (stat(filepath, &st) == 0) {
        is_gz = false;
        final_path = filepath;
    } else {
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

    if (strcmp(req->uri, "/favicon.ico") == 0 || strcmp(req->uri, "/favicon.svg") == 0) {
        struct stat st;
        if (stat("/spiffs/favicon.svg", &st) == 0 || stat("/spiffs/favicon.svg.gz", &st) == 0) {
            return serve_spiffs_file(req, "/spiffs/favicon.svg");
        }
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    // Always serve latest embedded single-page dashboard for root and client routes
    if (strcmp(req->uri, "/") == 0 || strstr(req->uri, "/assets/") == NULL) {
        return serve_embedded_index_html(req);
    }

    char filepath[160];
    const char *quest = strchr(req->uri, '?');
    size_t path_len = quest ? (size_t)(quest - req->uri) : strlen(req->uri);
    if (path_len >= sizeof(filepath) - 8) {
        path_len = sizeof(filepath) - 9;
    }

    snprintf(filepath, sizeof(filepath), "/spiffs%.*s", (int)path_len, req->uri);
    return serve_spiffs_file(req, filepath);
}

/* =========================================================================
 * Access Control & Web Authentication
 * ========================================================================= */

/* GET /api/auth/status */
static esp_err_t api_auth_status_handler(httpd_req_t *req) {
    bool is_wg = auth_is_trusted_tunnel(req);
    bool pass_set = auth_is_password_set();
    bool auth = auth_is_authenticated(req);

    int sockfd = httpd_req_to_sockfd(req);
    char local_ip[32] = "";
    char peer_ip[32] = "";
    if (sockfd >= 0) {
        struct sockaddr_in la = {0}, pa = {0};
        socklen_t l = sizeof(la), pl = sizeof(pa);
        if (getsockname(sockfd, (struct sockaddr *)&la, &l) == 0) {
            inet_ntop(AF_INET, &la.sin_addr, local_ip, sizeof(local_ip));
        }
        if (getpeername(sockfd, (struct sockaddr *)&pa, &pl) == 0) {
            inet_ntop(AF_INET, &pa.sin_addr, peer_ip, sizeof(peer_ip));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddBoolToObject(root, "is_wireguard", is_wg);
    cJSON_AddBoolToObject(root, "password_configured", pass_set);
    cJSON_AddBoolToObject(root, "setup_required", !pass_set);
    cJSON_AddBoolToObject(root, "authenticated", auth);
    cJSON_AddStringToObject(root, "local_ip", local_ip);
    cJSON_AddStringToObject(root, "client_ip", peer_ip);

    return http_send_json(req, "200 OK", root, true);
}

/* POST /api/auth/setup */
static esp_err_t api_auth_setup_handler(httpd_req_t *req) {
    if (!auth_validate_csrf(req)) return ESP_OK;

    if (auth_is_password_set()) {
        return http_send_error(req, "400 Bad Request", "Password already configured");
    }

    cJSON *root = http_parse_json_body(req, 512);
    if (!root) return ESP_FAIL;

    cJSON *p_item = cJSON_GetObjectItem(root, "password");
    const char *pass = (p_item && cJSON_IsString(p_item)) ? p_item->valuestring : "";

    if (strlen(pass) < 6) {
        cJSON_Delete(root);
        return http_send_error(req, "400 Bad Request", "Password must be at least 6 characters");
    }

    if (!auth_set_password(pass)) {
        cJSON_Delete(root);
        return http_send_error(req, "500 Internal Error", "Failed to store password");
    }

    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "token", auth_get_session_token());
    return http_send_json(req, "200 OK", resp, true);
}

/* POST /api/auth/login */
static esp_err_t api_auth_login_handler(httpd_req_t *req) {
    if (!auth_validate_csrf(req)) return ESP_OK;

    uint32_t wait_sec = 0;
    if (auth_is_locked_out(&wait_sec)) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Too many failed attempts. Try again in %lu seconds", (unsigned long)wait_sec);
        return http_send_error(req, "429 Too Many Requests", err_msg);
    }

    cJSON *root = http_parse_json_body(req, 512);
    if (!root) return ESP_FAIL;

    cJSON *p_item = cJSON_GetObjectItem(root, "password");
    const char *pass = (p_item && cJSON_IsString(p_item)) ? p_item->valuestring : "";

    if (!auth_is_password_set()) {
        cJSON_Delete(root);
        return http_send_error(req, "403 Forbidden", "Initial setup required");
    }

    if (auth_verify_password(pass)) {
        auth_reset_failed_attempts();
        auth_rotate_session_token();
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "token", auth_get_session_token());
        cJSON_Delete(root);
        return http_send_json(req, "200 OK", resp, true);
    } else {
        auth_record_failed_attempt();
        cJSON_Delete(root);
        return http_send_error(req, "401 Unauthorized", "Invalid password");
    }
}

/* POST /api/auth/password */
static esp_err_t api_auth_password_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    cJSON *root = http_parse_json_body(req, 1024);
    if (!root) return ESP_FAIL;

    cJSON *curr_item = cJSON_GetObjectItem(root, "current_password");
    cJSON *new_item = cJSON_GetObjectItem(root, "new_password");
    const char *current_pass = (curr_item && cJSON_IsString(curr_item)) ? curr_item->valuestring : "";
    const char *new_pass = (new_item && cJSON_IsString(new_item)) ? new_item->valuestring : "";

    bool is_wg = auth_is_trusted_tunnel(req);

    // If password is currently set and request is not via WireGuard, verify current password
    if (auth_is_password_set() && !is_wg) {
        if (!auth_verify_password(current_pass)) {
            cJSON_Delete(root);
            return http_send_error(req, "403 Forbidden", "Current password is incorrect");
        }
    }

    // New password length check (if setting a non-empty password)
    if (strlen(new_pass) > 0 && strlen(new_pass) < 6) {
        cJSON_Delete(root);
        return http_send_error(req, "400 Bad Request", "Password must be at least 6 characters");
    }

    if (!auth_set_password(new_pass)) {
        cJSON_Delete(root);
        return http_send_error(req, "500 Internal Error", "Failed to update password");
    }

    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "token", auth_get_session_token());
    return http_send_json(req, "200 OK", resp, true);
}

/* POST /api/wifi/ap */
static esp_err_t api_wifi_ap_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    bool enable = true;
    cJSON *root = http_parse_json_body(req, 256);
    if (root) {
        cJSON *en_item = cJSON_GetObjectItem(root, "enabled");
        if (en_item && cJSON_IsBool(en_item)) {
            enable = cJSON_IsTrue(en_item);
        }
        cJSON_Delete(root);
    }

    esp_err_t err;
    if (enable) {
        err = wifi_manager_enable_ap();
        if (err == ESP_OK) {
            dns_server_start();
        }
    } else {
        err = wifi_manager_disable_ap();
        if (err == ESP_OK) {
            dns_server_stop();
        }
    }

    if (err == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) return httpd_resp_send_500(req);
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "ap", enable ? "enabled" : "disabled");
        return http_send_json(req, "200 OK", resp, true);
    } else {
        return http_send_error(req, "500 Internal Error", "Failed to change AP state");
    }
}

/* =========================================================================
 * REST API: Gate Control
 * ========================================================================= */

/* POST /api/gate/garage */
static esp_err_t api_gate_garage_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    garage_action_t action = GARAGE_ACTION_FULL;
    cJSON *root = http_parse_json_body(req, 512);
    if (root) {
        cJSON *act_item = cJSON_GetObjectItem(root, "action");
        if (act_item && cJSON_IsString(act_item)) {
            if (strcmp(act_item->valuestring, "vent") == 0) {
                action = GARAGE_ACTION_VENT;
            } else if (strcmp(act_item->valuestring, "full") == 0) {
                action = GARAGE_ACTION_FULL;
            } else {
                cJSON_Delete(root);
                return http_send_error(req, "400 Bad Request", "Invalid garage action. Use 'full' or 'vent'");
            }
        }
        cJSON_Delete(root);
    }

    esp_err_t err = gate_garage_trigger(action);
    if (err == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) return httpd_resp_send_500(req);
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "action", (action == GARAGE_ACTION_VENT) ? "garage_vent" : "garage_full");
        return http_send_json(req, "200 OK", resp, true);
    } else if (err == ESP_ERR_INVALID_STATE) {
        return http_send_error(req, "429 Too Many Requests", "Interlock safety delay active");
    } else {
        return http_send_error(req, "500 Internal Error", "Failed to trigger garage gate");
    }
}

/* POST /api/gate/driveway */
static esp_err_t api_gate_driveway_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    driveway_action_t action = DRIVEWAY_ACTION_OPEN;
    cJSON *root = http_parse_json_body(req, 512);
    if (root) {
        cJSON *act_item = cJSON_GetObjectItem(root, "action");
        if (act_item && cJSON_IsString(act_item)) {
            if (strcmp(act_item->valuestring, "close") == 0) {
                action = DRIVEWAY_ACTION_CLOSE;
            } else if (strcmp(act_item->valuestring, "open") == 0) {
                action = DRIVEWAY_ACTION_OPEN;
            } else {
                cJSON_Delete(root);
                return http_send_error(req, "400 Bad Request", "Invalid driveway action. Use 'open' or 'close'");
            }
        }
        cJSON_Delete(root);
    }

    esp_err_t err = gate_driveway_trigger(action);
    if (err == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) return httpd_resp_send_500(req);
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "action", (action == DRIVEWAY_ACTION_CLOSE) ? "driveway_close" : "driveway_open");
        return http_send_json(req, "200 OK", resp, true);
    } else if (err == ESP_ERR_INVALID_STATE) {
        return http_send_error(req, "429 Too Many Requests", "Interlock safety delay active");
    } else {
        return http_send_error(req, "500 Internal Error", "Failed to trigger driveway gate");
    }
}

/* GET /api/gate/status */
static esp_err_t api_gate_status_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    gate_status_t st;
    gate_controller_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "garage_state", st.garage_state);
    cJSON_AddStringToObject(root, "garage_state_str", gate_state_to_str(st.garage_state));
    cJSON_AddNumberToObject(root, "driveway_state", st.driveway_state);
    cJSON_AddStringToObject(root, "driveway_state_str", gate_state_to_str(st.driveway_state));
    cJSON_AddNumberToObject(root, "pulse_duration_ms", (double)st.pulse_duration_ms);
    cJSON_AddNumberToObject(root, "interlock_delay_ms", (double)st.interlock_delay_ms);
    cJSON_AddNumberToObject(root, "last_garage_ts", (double)st.last_garage_action_ts);
    cJSON_AddNumberToObject(root, "last_driveway_ts", (double)st.last_driveway_action_ts);
    cJSON_AddStringToObject(root, "last_action", st.last_action_desc);

    return http_send_json(req, "200 OK", root, true);
}

/* POST /api/gate/settings */
static esp_err_t api_gate_settings_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    cJSON *root = http_parse_json_body(req, 512);
    if (!root) return ESP_FAIL;

    cJSON *pulse_item = cJSON_GetObjectItem(root, "pulse_ms");
    if (pulse_item && cJSON_IsNumber(pulse_item)) {
        uint32_t ms = (uint32_t)pulse_item->valuedouble;
        if (ms >= 100 && ms <= 2000) {
            gate_controller_set_pulse_duration(ms);
        } else {
            cJSON_Delete(root);
            return http_send_error(req, "400 Bad Request", "pulse_ms must be between 100 and 2000");
        }
    }

    cJSON *interlock_item = cJSON_GetObjectItem(root, "interlock_ms");
    if (interlock_item && cJSON_IsNumber(interlock_item)) {
        uint32_t ms = (uint32_t)interlock_item->valuedouble;
        if (ms >= 500 && ms <= 5000) {
            gate_controller_set_interlock_delay(ms);
        } else {
            cJSON_Delete(root);
            return http_send_error(req, "400 Bad Request", "interlock_ms must be between 500 and 5000");
        }
    }

    cJSON_Delete(root);
    return http_send_ok(req, "Gate timings updated");
}

/* =========================================================================
 * REST API: WireGuard VPN
 * ========================================================================= */

/* POST /api/wireguard/toggle */
static esp_err_t api_wireguard_toggle_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    bool enable = true;
    cJSON *root = http_parse_json_body(req, 256);
    if (root) {
        cJSON *en_item = cJSON_GetObjectItem(root, "enabled");
        if (en_item && cJSON_IsBool(en_item)) {
            enable = cJSON_IsTrue(en_item);
        }
        cJSON_Delete(root);
    }

    wireguard_manager_set_enabled(enable);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "vpn", enable ? "enabled" : "disabled");
    return http_send_json(req, "200 OK", resp, true);
}

/* POST /api/wireguard/config */
static esp_err_t api_wireguard_config_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    char *body = http_read_body(req, 4096);
    if (!body) return ESP_FAIL;

    esp_err_t err = wireguard_manager_set_config_from_text(body);
    free(body);

    if (err == ESP_OK) {
        return http_send_ok(req, "WireGuard configuration updated");
    } else {
        return http_send_error(req, "400 Bad Request", "Invalid WireGuard configuration format");
    }
}

/* =========================================================================
 * REST API: AWS Route 53
 * ========================================================================= */

/* POST /api/route53/sync */
static esp_err_t api_route53_sync_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    wireguard_status_t wg_st;
    wireguard_manager_get_status(&wg_st);

    const char *target_ip = wg_st.is_connected ? wg_st.assigned_ip : wifi_manager_get_sta_ip();
    esp_err_t err = aws_route53_sync_record(target_ip);

    if (err == ESP_OK) {
        return http_send_ok(req, "Route 53 DNS sync initiated");
    } else {
        return http_send_error(req, "400 Bad Request", "Route 53 not configured or missing IP");
    }
}

/* =========================================================================
 * REST API: Wi-Fi Management
 * ========================================================================= */

/* POST /api/wifi/connect */
static esp_err_t api_wifi_connect_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    cJSON *root = http_parse_json_body(req, 1024);
    if (!root) return ESP_FAIL;

    cJSON *s_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *p_item = cJSON_GetObjectItem(root, "password");
    const char *ssid = (s_item && cJSON_IsString(s_item)) ? s_item->valuestring : "";
    const char *pass = (p_item && cJSON_IsString(p_item)) ? p_item->valuestring : "";

    if (strlen(ssid) == 0) {
        cJSON_Delete(root);
        return http_send_error(req, "400 Bad Request", "SSID is required");
    }

    esp_err_t err = wifi_manager_set_sta_credentials(ssid, pass);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        return http_send_ok(req, "Connecting to Wi-Fi...");
    } else {
        return http_send_error(req, "500 Internal Error", "Failed to initiate connection");
    }
}

/* =========================================================================
 * REST API: System & OTA
 * ========================================================================= */

/* GET /api/system/info */
static esp_err_t api_system_info_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

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

    const char *wifi_mode_str = "OFF";
    if (wifi_cfg.ap_enabled && wifi_cfg.sta_enabled) {
        wifi_mode_str = "AP+STA";
    } else if (wifi_cfg.ap_enabled) {
        wifi_mode_str = "AP_ONLY";
    } else if (wifi_cfg.sta_enabled) {
        wifi_mode_str = "STA_ONLY";
    }

    bool is_wg = auth_is_trusted_tunnel(req);
    bool pass_configured = auth_is_password_set();

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    cJSON_AddStringToObject(root, "app", "VorotaBot-esp32");
    cJSON_AddStringToObject(root, "version", ota_st.current_app_version);
    cJSON_AddStringToObject(root, "chip", "ESP32-C3");
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    cJSON *auth_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(auth_obj, "is_wireguard", is_wg);
    cJSON_AddBoolToObject(auth_obj, "password_configured", pass_configured);
    cJSON_AddItemToObject(root, "auth", auth_obj);

    cJSON *wifi_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi_obj, "mode", wifi_mode_str);
    cJSON_AddBoolToObject(wifi_obj, "ap_enabled", wifi_cfg.ap_enabled);
    cJSON_AddBoolToObject(wifi_obj, "sta_connected", wifi_cfg.sta_connected);
    cJSON_AddStringToObject(wifi_obj, "sta_ssid", wifi_cfg.sta_ssid);
    cJSON_AddStringToObject(wifi_obj, "sta_ip", wifi_cfg.sta_ip);
    cJSON_AddNumberToObject(wifi_obj, "sta_rssi", wifi_manager_get_sta_rssi());
    cJSON_AddItemToObject(root, "wifi", wifi_obj);

    cJSON *wg_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(wg_obj, "configured", wg_st.is_configured);
    cJSON_AddBoolToObject(wg_obj, "enabled", wg_st.is_enabled);
    cJSON_AddBoolToObject(wg_obj, "connected", wg_st.is_connected);
    cJSON_AddStringToObject(wg_obj, "ip", wg_st.assigned_ip);
    cJSON_AddStringToObject(wg_obj, "endpoint", wg_st.endpoint);
    cJSON_AddItemToObject(root, "wireguard", wg_obj);

    cJSON *r53_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(r53_obj, "configured", r53_st.is_configured);
    cJSON_AddBoolToObject(r53_obj, "synced", r53_st.is_synced);
    cJSON_AddStringToObject(r53_obj, "fqdn", r53_st.fqdn);
    cJSON_AddItemToObject(root, "route53", r53_obj);

    cJSON *gates_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(gates_obj, "garage_state", gate_st.garage_state);
    cJSON_AddNumberToObject(gates_obj, "driveway_state", gate_st.driveway_state);
    cJSON_AddStringToObject(gates_obj, "last_action", gate_st.last_action_desc);
    cJSON_AddItemToObject(root, "gates", gates_obj);

    cJSON_AddStringToObject(root, "ota_partition", ota_st.current_partition_label);
    char build_time[64];
    snprintf(build_time, sizeof(build_time), "%s %s", __DATE__, __TIME__);
    cJSON_AddStringToObject(root, "build_time", build_time);

    return http_send_json(req, "200 OK", root, true);
}

/* POST /api/system/restart */
static esp_err_t api_system_restart_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    http_send_ok(req, "Rebooting ESP32...");
    ota_manager_reboot_delayed(800);
    return ESP_OK;
}

/* POST /api/system/factory-reset */
static esp_err_t api_system_factory_reset_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    nvs_manager_factory_reset();
    http_send_ok(req, "Factory reset complete. Rebooting...");
    ota_manager_reboot_delayed(800);
    return ESP_OK;
}

/* POST /api/system/ota */
static esp_err_t api_system_ota_handler(httpd_req_t *req) {
    if (!auth_check_or_reject(req)) return ESP_OK;

    ESP_LOGI(TAG, "Starting OTA stream upload (Total size: %d bytes)...", req->content_len);

    if (req->content_len <= 0) {
        http_send_error(req, "400 Bad Request", "Content-Length header required");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_begin(req->content_len);
    if (err != ESP_OK) {
        http_send_error(req, "500 Internal Error", "Failed to initialize OTA partition");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    int timeout_retries = 0;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT && timeout_retries++ < 10) {
                continue;
            }
            ESP_LOGE(TAG, "OTA receive timeout / error");
            ota_manager_abort();
            http_send_error(req, "500 Internal Error", "Upload transfer failed");
            return ESP_FAIL;
        }
        timeout_retries = 0;

        err = ota_manager_write(buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA flash write error");
            ota_manager_abort();
            http_send_error(req, "500 Internal Error", "Flash write failed");
            return ESP_FAIL;
        }

        remaining -= recv_len;
    }

    err = ota_manager_end();
    if (err != ESP_OK) {
        http_send_error(req, "500 Internal Error", "Firmware verification failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Update complete! Rebooting in 1.5s...");
    http_send_ok(req, "OTA Flash successful! Device rebooting...");

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
    auth_init();
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

        // Auth & Access Control Routes
        httpd_uri_t uri_auth_st = { .uri = "/api/auth/status", .method = HTTP_GET, .handler = api_auth_status_handler };
        httpd_register_uri_handler(s_server, &uri_auth_st);

        httpd_uri_t uri_auth_setup = { .uri = "/api/auth/setup", .method = HTTP_POST, .handler = api_auth_setup_handler };
        httpd_register_uri_handler(s_server, &uri_auth_setup);

        httpd_uri_t uri_auth_login = { .uri = "/api/auth/login", .method = HTTP_POST, .handler = api_auth_login_handler };
        httpd_register_uri_handler(s_server, &uri_auth_login);

        httpd_uri_t uri_auth_pass = { .uri = "/api/auth/password", .method = HTTP_POST, .handler = api_auth_password_handler };
        httpd_register_uri_handler(s_server, &uri_auth_pass);

        // Wi-Fi SoftAP Runtime Toggle Route
        httpd_uri_t uri_wifi_ap = { .uri = "/api/wifi/ap", .method = HTTP_POST, .handler = api_wifi_ap_handler };
        httpd_register_uri_handler(s_server, &uri_wifi_ap);

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
