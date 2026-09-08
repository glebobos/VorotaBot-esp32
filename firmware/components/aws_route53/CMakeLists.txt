idf_component_register(SRCS "aws_route53.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_manager esp_http_client mbedtls esp-tls freertos esp_timer log)
