#pragma once

#include "esp_http_server.h"
#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the entire HTTP request body into a newly allocated, null-terminated buffer.
 * If content_len > max_len, automatically sends 413 Payload Too Large and returns NULL.
 * Returns NULL on error or timeout. Caller must free() the returned buffer.
 */
char* http_read_body(httpd_req_t *req, size_t max_len);

/**
 * Reads request body and parses it as cJSON.
 * Sends 400 Bad Request if parsing fails or body missing.
 * Caller must cJSON_Delete() the returned root object.
 */
cJSON* http_parse_json_body(httpd_req_t *req, size_t max_len);

/**
 * Send a cJSON object as an application/json response.
 * If status_str is NULL, "200 OK" is used.
 * If delete_root is true, cJSON_Delete(root) is called automatically.
 */
esp_err_t http_send_json(httpd_req_t *req, const char *status_str, cJSON *root, bool delete_root);

/**
 * Send standard error response:
 * {"status":"error","message":"<msg>"}
 */
esp_err_t http_send_error(httpd_req_t *req, const char *status_str, const char *msg);

/**
 * Send standard ok response:
 * {"status":"ok","message":"<msg>"} (message omitted if NULL)
 */
esp_err_t http_send_ok(httpd_req_t *req, const char *msg);

#ifdef __cplusplus
}
#endif
