idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event log nvs_manager esp_timer
)
