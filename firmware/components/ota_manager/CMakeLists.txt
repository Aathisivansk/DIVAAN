idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include" "../../include"
    REQUIRES app_update esp_http_client esp_https_ota esp-tls json fatfs display_driver
)