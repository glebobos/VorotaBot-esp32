#include "ble_service.h"
#include "wifi_manager.h"
#include "gate_controller.h"
#include "nvs_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_SERVICE";

// 128-bit UUIDs for VorotaBot Service & Characteristics
// Service: 0000ff00-0000-1000-8000-00805f9b34fb
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00);

// Char 1: WiFi SSID (Write/Read)
static const ble_uuid128_t s_char_wifi_ssid_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00);

// Char 2: WiFi Password (Write-only)
static const ble_uuid128_t s_char_wifi_pass_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00);

// Char 3: Status (Read/Notify)
static const ble_uuid128_t s_char_status_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0x00, 0x00);

// Char 4: WireGuard VPN Toggle (Read/Write/Notify)
static const ble_uuid128_t s_char_vpn_toggle_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0xff, 0x00, 0x00);

// Char 5: Gate Trigger (Write-only)
static const ble_uuid128_t s_char_gate_cmd_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x05, 0xff, 0x00, 0x00);

static uint16_t s_status_val_handle;
static uint16_t s_vpn_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_vpn_enabled = false;
static ble_vpn_toggle_cb_t s_vpn_cb = NULL;

static char s_pending_ssid[33] = {0};
static char s_pending_pass[65] = {0};

/* Forward declarations */
static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // WiFi SSID
                .uuid = &s_char_wifi_ssid_uuid.u,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // WiFi Password
                .uuid = &s_char_wifi_pass_uuid.u,
                .flags = BLE_GATT_CHR_F_WRITE,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // Connection Status
                .uuid = &s_char_status_uuid.u,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // WireGuard Toggle
                .uuid = &s_char_vpn_toggle_uuid.u,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_vpn_val_handle,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // Gate Command Trigger
                .uuid = &s_char_gate_cmd_uuid.u,
                .flags = BLE_GATT_CHR_F_WRITE,
                .access_cb = gatt_svr_access_cb,
            },
            {
                0, // End of characteristics
            }
        },
    },
    {
        0, // End of services
    },
};

static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    // 1. WiFi SSID
    if (ble_uuid_cmp(uuid, &s_char_wifi_ssid_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            wifi_mgr_config_t cfg;
            wifi_manager_get_config(&cfg);
            os_mbuf_append(ctxt->om, cfg.sta_ssid, strlen(cfg.sta_ssid));
            return 0;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len >= sizeof(s_pending_ssid)) len = sizeof(s_pending_ssid) - 1;
            os_mbuf_copydata(ctxt->om, 0, len, s_pending_ssid);
            s_pending_ssid[len] = '\0';
            ESP_LOGI(TAG, "BLE received WiFi SSID: %s", s_pending_ssid);
            return 0;
        }
    }

    // 2. WiFi Password
    if (ble_uuid_cmp(uuid, &s_char_wifi_pass_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len >= sizeof(s_pending_pass)) len = sizeof(s_pending_pass) - 1;
            os_mbuf_copydata(ctxt->om, 0, len, s_pending_pass);
            s_pending_pass[len] = '\0';
            ESP_LOGI(TAG, "BLE received WiFi Password (len: %d)", (int)len);

            if (strlen(s_pending_ssid) > 0) {
                ESP_LOGI(TAG, "Provisioning WiFi via BLE: SSID='%s'", s_pending_ssid);
                wifi_manager_set_sta_credentials(s_pending_ssid, s_pending_pass);
            }
            return 0;
        }
    }

    // 3. Status
    if (ble_uuid_cmp(uuid, &s_char_status_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            char status_buf[96];
            bool connected = wifi_manager_is_sta_connected();
            snprintf(status_buf, sizeof(status_buf), "wifi:%s,ip:%s,vpn:%s",
                     connected ? "connected" : "disconnected",
                     wifi_manager_get_sta_ip(),
                     s_vpn_enabled ? "active" : "disabled");
            os_mbuf_append(ctxt->om, status_buf, strlen(status_buf));
            return 0;
        }
    }

    // 4. WireGuard VPN Toggle
    if (ble_uuid_cmp(uuid, &s_char_vpn_toggle_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            char vpn_str[2] = { s_vpn_enabled ? '1' : '0', '\0' };
            os_mbuf_append(ctxt->om, vpn_str, 1);
            return 0;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            char buf[4] = {0};
            os_mbuf_copydata(ctxt->om, 0, 1, buf);
            bool enable = (buf[0] == '1' || buf[0] == 't' || buf[0] == 'y');
            ESP_LOGI(TAG, "BLE requested WireGuard toggle: %s", enable ? "ENABLE" : "DISABLE");
            s_vpn_enabled = enable;
            if (s_vpn_cb) {
                s_vpn_cb(enable);
            }
            ble_service_set_vpn_state(enable);
            return 0;
        }
    }

    // 5. Gate Trigger
    if (ble_uuid_cmp(uuid, &s_char_gate_cmd_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            char cmd[32] = {0};
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
            os_mbuf_copydata(ctxt->om, 0, len, cmd);
            cmd[len] = '\0';

            ESP_LOGI(TAG, "BLE Gate Command received: '%s'", cmd);

            if (strcmp(cmd, "garage_full") == 0) {
                gate_garage_trigger(GARAGE_ACTION_FULL);
            } else if (strcmp(cmd, "garage_vent") == 0) {
                gate_garage_trigger(GARAGE_ACTION_VENT);
            } else if (strcmp(cmd, "driveway_open") == 0) {
                gate_driveway_trigger(DRIVEWAY_ACTION_OPEN);
            } else if (strcmp(cmd, "driveway_close") == 0) {
                gate_driveway_trigger(DRIVEWAY_ACTION_CLOSE);
            } else {
                ESP_LOGW(TAG, "Unknown BLE gate command: %s", cmd);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE Client connected (handle: %d)", s_conn_handle);
            } else {
                ESP_LOGW(TAG, "BLE Connection failed; resuming advertisement");
                ble_advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE Client disconnected; resuming advertisement");
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_advertise();
            break;

        default:
            break;
    }
    return 0;
}

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static void ble_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"VorotaBot";
    fields.name_len = strlen("VorotaBot");
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement fields; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting advertisement; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE Advertising started as 'VorotaBot'");
    }
}

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining address type; rc=%d", rc);
        return;
    }
    ble_advertise();
}

static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_service_init(ble_vpn_toggle_cb_t vpn_cb) {
    s_vpn_cb = vpn_cb;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE port: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    ble_svc_gap_device_name_set("VorotaBot");

    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "NimBLE Bluetooth Service Initialized");
    return ESP_OK;
}

void ble_service_notify_status(const char *status_str) {
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE && status_str != NULL) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(status_str, strlen(status_str));
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
        }
    }
}

void ble_service_set_vpn_state(bool enabled) {
    s_vpn_enabled = enabled;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        char val = enabled ? '1' : '0';
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, 1);
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_vpn_val_handle, om);
        }
    }
}

bool ble_service_is_connected(void) {
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}
