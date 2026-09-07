idf_component_register(SRCS "ble_service.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt wifi_manager gate_controller nvs_manager)
