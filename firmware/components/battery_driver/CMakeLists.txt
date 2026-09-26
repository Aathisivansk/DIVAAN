idf_component_register(SRCS "battery_driver.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_adc" "esp_timer" "driver")