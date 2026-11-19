idf_component_register(SRCS "gate_controller.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos esp_timer nvs_manager)
