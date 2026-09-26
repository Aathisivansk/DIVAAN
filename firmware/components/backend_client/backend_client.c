#include "backend_client.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "speaker_driver.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "BACKEND_CLIENT";
static char s_base_url[128] = {0};

esp_err_t backend_client_init(const char *base_url) {
    if (!base_url) return ESP_ERR_INVALID_ARG;
    strncpy(s_base_url, base_url, sizeof(s_base_url) - 1);
    size_t len = strlen(s_base_url);
    if (len > 0 && s_base_url[len - 1] == '/') {
        s_base_url[len - 1] = '\0';
    }
    ESP_LOGI(TAG, "Backend Client configured for: %s", s_base_url);
    return ESP_OK;
}

void backend_client_free_result(emo_api_result_t *result) {
    (void)result;
}

esp_err_t backend_client_send_media_cmd(const char *cmd_str) {
    if (!cmd_str) return ESP_ERR_INVALID_ARG;
    char url[384];
    snprintf(url, sizeof(url), "%s/api/bot/media_cmd?cmd=%s", s_base_url, cmd_str);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    esp_http_client_set_header(client, "User-Agent", "DivaanBot/1.0");
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t backend_client_poll_action(char *out_action, size_t max_len) {
    if (!out_action || max_len == 0) return ESP_ERR_INVALID_ARG;
    out_action[0] = '\0';
    
    char url[384];
    snprintf(url, sizeof(url), "%s/api/emo/poll_action", s_base_url);
    char response_buffer[512] = {0};

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 6000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = 1024,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    esp_http_client_set_header(client, "User-Agent", "DivaanBot/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int read_len = esp_http_client_read(client, response_buffer, sizeof(response_buffer) - 1);
        if (read_len > 0) {
            response_buffer[read_len] = '\0';
            cJSON *root = cJSON_Parse(response_buffer);
            if (root) {
                cJSON *act = cJSON_GetObjectItem(root, "action");
                if (act && act->valuestring && strlen(act->valuestring) > 0) {
                    strncpy(out_action, act->valuestring, max_len - 1);
                    out_action[max_len - 1] = '\0';
                    ESP_LOGI(TAG, "Polled Action: '%s'", out_action);
                }
                cJSON_Delete(root);
            }
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t backend_client_send_audio_sd(const char *sd_filepath, size_t file_len, emo_api_result_t *result) {
    if (!sd_filepath || file_len < 1000 || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(emo_api_result_t));
    result->emotion = BACKEND_EMOTION_NEUTRAL;

    FILE *f_in = fopen(sd_filepath, "rb");
    if (!f_in) {
        ESP_LOGE(TAG, "Cannot open SD audio file: %s", sd_filepath);
        return ESP_ERR_NOT_FOUND;
    }

    char full_url[384];
    snprintf(full_url, sizeof(full_url), "%s/api/emo/chat", s_base_url);

    esp_http_client_config_t config = {
        .url = full_url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 35000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f_in);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "DivaanBot/1.0");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, file_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        fclose(f_in);
        return err;
    }

    uint8_t chunk_buf[1024];
    size_t total_written = 0;
    while (total_written < file_len) {
        size_t bytes_read = fread(chunk_buf, 1, sizeof(chunk_buf), f_in);
        if (bytes_read == 0) break;
        int wlen = esp_http_client_write(client, (const char *)chunk_buf, bytes_read);
        if (wlen <= 0) break;
        total_written += wlen;
    }
    fclose(f_in);

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    char header_val[128] = {0};
    if (esp_http_client_get_header(client, "X-Emo-Reply", (char **)&header_val) == ESP_OK && header_val[0] != '\0') {
        strncpy(result->reply, header_val, sizeof(result->reply) - 1);
    }
    if (esp_http_client_get_header(client, "X-Emo-Emotion", (char **)&header_val) == ESP_OK && header_val[0] != '\0') {
        if (strcasecmp(header_val, "HAPPY") == 0) result->emotion = BACKEND_EMOTION_HAPPY;
        else if (strcasecmp(header_val, "SAD") == 0) result->emotion = BACKEND_EMOTION_SAD;
        else result->emotion = BACKEND_EMOTION_NEUTRAL;
    }

    if (status_code == 200) {
        while (1) {
            int rlen = esp_http_client_read(client, (char *)chunk_buf, sizeof(chunk_buf));
            if (rlen <= 0) break;
            speaker_driver_play_raw(chunk_buf, rlen);
        }
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

esp_err_t backend_client_send_audio_psram(const uint8_t *pcm_data, size_t data_len, emo_api_result_t *result) {
    if (!pcm_data || data_len < 1000 || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(emo_api_result_t));
    result->emotion = BACKEND_EMOTION_NEUTRAL;

    char full_url[384];
    snprintf(full_url, sizeof(full_url), "%s/api/emo/chat", s_base_url);

    esp_http_client_config_t config = {
        .url = full_url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 35000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "User-Agent", "DivaanBot/1.0");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, data_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    size_t total_written = 0;
    while (total_written < data_len) {
        size_t chunk = data_len - total_written;
        if (chunk > 1024) chunk = 1024;
        int wlen = esp_http_client_write(client, (const char *)(pcm_data + total_written), chunk);
        if (wlen <= 0) break;
        total_written += wlen;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    char header_val[128] = {0};
    if (esp_http_client_get_header(client, "X-Emo-Reply", (char **)&header_val) == ESP_OK && header_val[0] != '\0') {
        strncpy(result->reply, header_val, sizeof(result->reply) - 1);
    }
    if (esp_http_client_get_header(client, "X-Emo-Emotion", (char **)&header_val) == ESP_OK && header_val[0] != '\0') {
        if (strcasecmp(header_val, "HAPPY") == 0) result->emotion = BACKEND_EMOTION_HAPPY;
        else if (strcasecmp(header_val, "SAD") == 0) result->emotion = BACKEND_EMOTION_SAD;
        else result->emotion = BACKEND_EMOTION_NEUTRAL;
    }

    if (status_code == 200) {
        uint8_t stream_buf[512];
        while (1) {
            int rlen = esp_http_client_read(client, (char *)stream_buf, sizeof(stream_buf));
            if (rlen <= 0) break;
            speaker_driver_play_raw(stream_buf, rlen);
        }
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}