#include "http_util.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "HTTP_UTIL";

char* http_read_body(httpd_req_t *req, size_t max_len) {
    if (!req) return NULL;

    if (req->content_len <= 0) {
        http_send_error(req, "400 Bad Request", "Empty request body");
        return NULL;
    }

    if ((size_t)req->content_len > max_len) {
        ESP_LOGW(TAG, "Request payload too large (%d > %u)", req->content_len, (unsigned int)max_len);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Payload too large\"}");
        return NULL;
    }

    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for body", req->content_len + 1);
        httpd_resp_send_500(req);
        return NULL;
    }

    int remaining = req->content_len;
    int received_total = 0;
    int retry_count = 0;

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received_total, remaining);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && retry_count++ < 3) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
            free(buf);
            http_send_error(req, "500 Internal Error", "Failed to receive request body");
            return NULL;
        }
        received_total += ret;
        remaining -= ret;
    }

    buf[received_total] = '\0';
    return buf;
}

cJSON* http_parse_json_body(httpd_req_t *req, size_t max_len) {
    char *body = http_read_body(req, max_len);
    if (!body) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        ESP_LOGW(TAG, "cJSON_Parse failed on request body");
        http_send_error(req, "400 Bad Request", "Invalid JSON format");
        return NULL;
    }

    return root;
}

esp_err_t http_send_json(httpd_req_t *req, const char *status_str, cJSON *root, bool delete_root) {
    if (!req) {
        if (delete_root && root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (status_str) {
        httpd_resp_set_status(req, status_str);
    } else {
        httpd_resp_set_status(req, "200 OK");
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t res = ESP_FAIL;
    if (root) {
        char *str = cJSON_PrintUnformatted(root);
        if (str) {
            res = httpd_resp_sendstr(req, str);
            free(str);
        } else {
            ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
            res = httpd_resp_send_500(req);
        }
        if (delete_root) {
            cJSON_Delete(root);
        }
    } else {
        res = httpd_resp_sendstr(req, "{}");
    }

    return res;
}

esp_err_t http_send_error(httpd_req_t *req, const char *status_str, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg ? msg : "Unknown error");
    return http_send_json(req, status_str ? status_str : "400 Bad Request", root, true);
}

esp_err_t http_send_ok(httpd_req_t *req, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "status", "ok");
    if (msg) {
        cJSON_AddStringToObject(root, "message", msg);
    }
    return http_send_json(req, "200 OK", root, true);
}
