idf_component_register(SRCS "wireguard_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_manager lwip esp_netif freertos esp_timer log trombik__esp_wireguard)
