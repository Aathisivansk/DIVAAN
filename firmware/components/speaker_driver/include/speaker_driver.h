#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t speaker_driver_init(int gpio_dout_num);
void speaker_driver_play_raw(const uint8_t *data, size_t size);
void speaker_driver_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void speaker_driver_play_base64(const char *base64_str);
void speaker_driver_play_wav(const char *filepath);
void speaker_play_sd_pcm(const char *filepath);

#ifdef __cplusplus
}
#endif