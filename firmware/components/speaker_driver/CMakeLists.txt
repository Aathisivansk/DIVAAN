idf_component_register(
    SRCS "speaker_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_sdm esp_driver_gpio esp_timer
)