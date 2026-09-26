#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate;
    uint8_t record_seconds;
} audio_recorder_config_t;

esp_err_t audio_recorder_init(const audio_recorder_config_t *config);
esp_err_t audio_recorder_start(void);
void audio_recorder_stop(void);
bool audio_recorder_is_active(void);
uint8_t* audio_recorder_get_psram_buffer(size_t *out_len);

#ifdef __cplusplus
}
#endif