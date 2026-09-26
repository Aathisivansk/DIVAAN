idf_component_register(
    SRCS "groq_stt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json
)