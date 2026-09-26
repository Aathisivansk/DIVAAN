#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *api_key;
    const char *host;
    const char *endpoint;
} groq_stt_config_t;

esp_err_t groq_stt_init(const groq_stt_config_t *config);
esp_err_t groq_stt_transcribe(const uint8_t *audio_data, size_t data_len, char *out_text, size_t max_out_len);

#ifdef __cplusplus
}
#endif