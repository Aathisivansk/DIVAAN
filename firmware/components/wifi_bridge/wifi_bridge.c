#include "wifi_bridge.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WIFI_BRIDGE";
static char s_gateway_ip[32] = "192.168.43.1"; // Default Android Hotspot Gateway

void wifi_bridge_set_gateway_ip(const char *gateway_ip) {
    if (gateway_ip && strlen(gateway_ip) > 0) {
        strncpy(s_gateway_ip, gateway_ip, sizeof(s_gateway_ip) - 1);
        ESP_LOGI(TAG, "Phone Gateway IP set to: %s", s_gateway_ip);
    }
}

esp_err_t wifi_bridge_send_media_cmd(wifi_media_cmd_t cmd) {
    char url[128];
    const char *endpoint = "play_pause";
    switch (cmd) {
        case WIFI_MEDIA_PLAY_PAUSE: endpoint = "play_pause"; break;
        case WIFI_MEDIA_NEXT:       endpoint = "next"; break;
        case WIFI_MEDIA_PREV:       endpoint = "prev"; break;
        case WIFI_MEDIA_VOL_UP:     endpoint = "vol_up"; break;
        case WIFI_MEDIA_VOL_DOWN:   endpoint = "vol_down"; break;
    }

    snprintf(url, sizeof(url), "http://%s:8080/media/%s", s_gateway_ip, endpoint);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 800, // Instant non-blocking local UDP/TCP burst
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err;
}