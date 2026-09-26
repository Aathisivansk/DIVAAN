idf_component_register(
    SRCS "touch_sensor.c"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}/include"
    REQUIRES esp_driver_gpio esp_timer
)