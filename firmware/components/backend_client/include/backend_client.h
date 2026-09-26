#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACKEND_EMOTION_NEUTRAL = 0,
    BACKEND_EMOTION_HAPPY,
    BACKEND_EMOTION_SAD
} backend_emotion_t;

typedef struct {
    char reply[128];
    backend_emotion_t emotion;
} emo_api_result_t;

esp_err_t backend_client_init(const char *base_url);
esp_err_t backend_client_send_audio_sd(const char *sd_filepath, size_t file_len, emo_api_result_t *result);
void backend_client_free_result(emo_api_result_t *result);
esp_err_t backend_client_poll_action(char *out_action, size_t max_len);
esp_err_t backend_client_send_audio_psram(const uint8_t *pcm_data, size_t data_len, emo_api_result_t *result);

esp_err_t backend_client_send_media_cmd(const char *cmd_str);

#ifdef __cplusplus
}
#endif