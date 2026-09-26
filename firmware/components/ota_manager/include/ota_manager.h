#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char version[16];
    char changelog[64];
    char binary_url[128];
    bool update_available;
} ota_info_t;

esp_err_t ota_check_update_info(const char *manifest_url, ota_info_t *out_info);
esp_err_t ota_perform_firmware_update(const char *firmware_url);