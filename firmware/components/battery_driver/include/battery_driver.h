#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#define BATTERY_ADC_PIN GPIO_NUM_14

typedef struct {
    float voltage;       // Actual battery pack voltage (e.g., 7.82V)
    int bars;            // 0 to 4 bars
    bool is_charging;    // True if charging detected
    bool is_critical;    // True if < 6.6V
} battery_status_t;

esp_err_t battery_driver_init(void);
battery_status_t battery_get_status(void);