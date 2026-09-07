idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server spiffs esp_timer log esp_wifi esp_netif wifi_manager nvs_manager ota_manager gate_controller wireguard_manager aws_route53
)
