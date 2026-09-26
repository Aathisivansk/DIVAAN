idf_component_register(
    SRCS "display_driver.c"
    INCLUDE_DIRS "include" "../../include"
    REQUIRES esp_lcd driver fatfs vfs sdmmc speaker_driver battery_driver
)