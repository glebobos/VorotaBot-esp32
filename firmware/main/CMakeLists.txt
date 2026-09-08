idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer log nvs_flash esp_event esp_netif esp_wifi
             dns_server wifi_manager nvs_manager ota_manager web_server
             gate_controller wireguard_manager aws_route53
)
