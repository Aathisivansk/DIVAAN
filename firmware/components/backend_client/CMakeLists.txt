idf_component_register(
    SRCS "backend_client.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}/include"
    REQUIRES esp_http_client esp-tls json mbedtls speaker_driver
)