idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition log esp_timer esp_system
)
