#include <stdio.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"

// Core & Networking Components
#include "nvs_manager.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include "ota_manager.h"
#include "web_server.h"

// VorotaBot Custom Components
#include "gate_controller.h"
#include "ble_service.h"
#include "wireguard_manager.h"
#include "aws_route53.h"

static const char *TAG = "VOROTA_MAIN";

/* Callback when WireGuard connects and acquires tunnel IP */
static void on_wireguard_connected(const char *tunnel_ip) {
    ESP_LOGI(TAG, "WireGuard tunnel is active with IP: %s. Initiating Route 53 DNS sync...", tunnel_ip);
    ble_service_set_vpn_state(true);

    char notify_buf[64];
    snprintf(notify_buf, sizeof(notify_buf), "vpn_connected,ip:%s", tunnel_ip);
    ble_service_notify_status(notify_buf);

    // Register hostname in Route 53 pointing to WireGuard IP
    aws_route53_sync_record(tunnel_ip);
}

/* Callback when BLE client toggles VPN state */
static void on_ble_vpn_toggle(bool enable) {
    ESP_LOGI(TAG, "BLE toggled WireGuard VPN: %s", enable ? "ENABLE" : "DISABLE");
    wireguard_manager_set_enabled(enable);
}

/* Wi-Fi and IP Event Handler */
static void wifi_ip_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi Station connected with IP: %s", ip_str);

        // Notify WireGuard manager that Internet connectivity is ready
        wireguard_manager_on_wifi_connected();

        char notify_buf[64];
        snprintf(notify_buf, sizeof(notify_buf), "wifi_connected,ip:%s", ip_str);
        ble_service_notify_status(notify_buf);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi Station disconnected");
        wireguard_manager_on_wifi_disconnected();
        ble_service_notify_status("wifi_disconnected");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  %s (v%s)", APP_NAME, APP_VERSION);
    ESP_LOGI(TAG, "  Dual Gate Controller (Hörmann & Nice Dual Relay Modules)");
    ESP_LOGI(TAG, "  Seeed Studio XIAO ESP32-C3 Platform");
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize Non-Volatile Storage (NVS)
    ESP_ERROR_CHECK(nvs_manager_init());

    // 2. Initialize Default Event Loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register event listeners for Wi-Fi and IP events
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip_event_handler, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_ip_event_handler, NULL, NULL);

    // 3. Initialize Gate Hardware Controller (Hörmann & Nice Relay Modules)
    ESP_ERROR_CHECK(gate_controller_init());

    // 4. Initialize AWS Route 53 dynamic DNS updater
    ESP_ERROR_CHECK(aws_route53_init());

    // 5. Initialize WireGuard VPN Manager
    ESP_ERROR_CHECK(wireguard_manager_init(on_wireguard_connected));

    // 6. Initialize NimBLE Bluetooth Provisioning & Remote Toggle Service
    ESP_ERROR_CHECK(ble_service_init(on_ble_vpn_toggle));

    // 7. Initialize OTA Manager
    ESP_ERROR_CHECK(ota_manager_init());

    // 8. Start Wi-Fi Subsystem (SoftAP + STA fallback)
    ESP_ERROR_CHECK(wifi_manager_init(CONFIG_VOROTABOT_WIFI_SSID, CONFIG_VOROTABOT_WIFI_PASSWORD));

    // 9. Start Captive Portal DNS Server (UDP Port 53)
    ESP_ERROR_CHECK(dns_server_start());

    // 10. Start HTTP Web Server (Port 80)
    web_server_config_t ws_cfg = {
        .port = 80,
        .max_open_sockets = 8
    };
    ESP_ERROR_CHECK(web_server_start(&ws_cfg));

    // 11. System Ready Banner
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  VOROTABOT READY & ONLINE");
    ESP_LOGI(TAG, "  SoftAP SSID:   %s", CONFIG_VOROTABOT_WIFI_SSID);
    ESP_LOGI(TAG, "  Local Web URL: http://192.168.4.1/");
    ESP_LOGI(TAG, "  Local Domain:  http://%s/", CONFIG_VOROTABOT_PORTAL_DOMAIN);
    ESP_LOGI(TAG, "  BLE Name:      VorotaBot (Provisioning / VPN toggle)");
    ESP_LOGI(TAG, "=================================================");
}
