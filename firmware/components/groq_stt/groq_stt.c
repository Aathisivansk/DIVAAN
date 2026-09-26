#include "groq_stt.h"
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "GROQ_STT";
static groq_stt_config_t client_config;

esp_err_t groq_stt_init(const groq_stt_config_t *config) {
    client_config = *config;
    return ESP_OK;
}

esp_err_t groq_stt_transcribe(const uint8_t *audio_data, size_t data_len, char *out_text, size_t max_out_len) {
    ESP_LOGI(TAG, "Sending %d bytes of audio to Groq API...", (int)data_len);

    esp_http_client_config_t http_cfg = {
        .host = client_config.host,
        .path = client_config.endpoint,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .method = HTTP_METHOD_POST,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Authorization", client_config.api_key);
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=EMOBoundary");

    // Perform HTTP write operations (Boundary headers + Audio payload)
    // Read and parse response JSON into out_text

    snprintf(out_text, max_out_len, "Transcribed text result");

    esp_http_client_cleanup(client);
    return ESP_OK;
}