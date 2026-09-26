#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MEDIA_PLAY_PAUSE = 0,
    WIFI_MEDIA_NEXT,
    WIFI_MEDIA_PREV,
    WIFI_MEDIA_VOL_UP,
    WIFI_MEDIA_VOL_DOWN
} wifi_media_cmd_t;

void wifi_bridge_set_gateway_ip(const char *gateway_ip);
esp_err_t wifi_bridge_send_media_cmd(wifi_media_cmd_t cmd);

#ifdef __cplusplus
}
#endif