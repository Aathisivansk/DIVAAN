#include "ota_manager.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "OTA_MGR";

#define CURRENT_FIRMWARE_VERSION "v1.2.0"

esp_err_t ota_check_update_info(const char *manifest_url, ota_info_t *out_info) {
    if (!manifest_url || !out_info) return ESP_ERR_INVALID_ARG;
    memset(out_info, 0, sizeof(ota_info_t));

    char response_buf[1024] = {0};
    esp_http_client_config_t cfg = {
        .url = manifest_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 6000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int len = esp_http_client_read(client, response_buf, sizeof(response_buf) - 1);
        if (len > 0) response_buf[len] = '\0';
        esp_http_client_cleanup(client);
    } else {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response_buf);
    if (!root) return ESP_FAIL;

    cJSON *ver = cJSON_GetObjectItem(root, "version");
    cJSON *notes = cJSON_GetObjectItem(root, "notes");
    cJSON *url = cJSON_GetObjectItem(root, "firmware_url");

    if (ver && ver->valuestring && url && url->valuestring) {
        strncpy(out_info->version, ver->valuestring, sizeof(out_info->version) - 1);
        strncpy(out_info->binary_url, url->valuestring, sizeof(out_info->binary_url) - 1);
        if (notes && notes->valuestring) {
            strncpy(out_info->changelog, notes->valuestring, sizeof(out_info->changelog) - 1);
        } else {
            strncpy(out_info->changelog, "Performance & Bug Fixes", sizeof(out_info->changelog) - 1);
        }

        if (strcmp(out_info->version, CURRENT_FIRMWARE_VERSION) != 0) {
            out_info->update_available = true;
            ESP_LOGI(TAG, "New firmware available: %s (Current: %s)", out_info->version, CURRENT_FIRMWARE_VERSION);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ota_perform_firmware_update(const char *firmware_url) {
    if (!firmware_url) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Starting OTA Download from: %s", firmware_url);

    esp_http_client_config_t http_cfg = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 25000,
        .keep_alive_enable = false,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeeded! Restarting system...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Download Failed: %s", esp_err_to_name(ret));
    }
    return ret;
}