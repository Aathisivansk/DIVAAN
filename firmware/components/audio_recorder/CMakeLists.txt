idf_component_register(
    SRCS "audio_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_driver_gpio esp_ringbuf
)