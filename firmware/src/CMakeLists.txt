idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include"
    REQUIRES 
        display_driver
        touch_sensor
        audio_recorder
        backend_client
        speaker_driver
        battery_driver
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        ota_manager
        fatfs
        vfs
        sdmmc
)