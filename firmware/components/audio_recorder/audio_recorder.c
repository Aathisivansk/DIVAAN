#include "audio_recorder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "AUDIO_REC_PSRAM";

#define ADC_UNIT            ADC_UNIT_1
#define ADC_CHANNEL         ADC_CHANNEL_3 // GPIO 4

static adc_continuous_handle_t adc_handle = NULL;
static size_t s_total_target_bytes = 0;
static volatile size_t s_actual_bytes = 0;
static volatile bool s_is_recording = false;
static volatile bool s_task_running = false;
static SemaphoreHandle_t s_sync_sem = NULL;
static uint32_t s_sample_rate = 16000;

// Dedicated 160 KB PSRAM Audio Buffer
static uint8_t *s_psram_audio_buf = NULL;

static void audio_record_task(void *pvParameters) {
    uint8_t raw_conv[512];
    int16_t pcm_chunk[256];

    while (1) {
        if (s_is_recording) {
            s_task_running = true;
            s_actual_bytes = 0;

            esp_err_t start_ret = adc_continuous_start(adc_handle);
            if (start_ret != ESP_OK) {
                ESP_LOGE(TAG, "ADC continuous start failed: %s", esp_err_to_name(start_ret));
                s_is_recording = false;
                s_task_running = false;
                continue;
            }

            while (s_is_recording && (s_actual_bytes + sizeof(pcm_chunk) <= s_total_target_bytes)) {
                uint32_t ret_num = 0;
                esp_err_t ret = adc_continuous_read(adc_handle, raw_conv, sizeof(raw_conv), &ret_num, 25);
                if (ret == ESP_OK && ret_num > 0) {
                    int pcm_idx = 0;
                    for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&raw_conv[i];
                        uint32_t raw_val = p->type2.data;
                        int32_t centered = ((int32_t)raw_val - 2048) * 16;
                        if (centered > 32767) centered = 32767;
                        if (centered < -32768) centered = -32768;
                        pcm_chunk[pcm_idx++] = (int16_t)centered;
                    }

                    if (pcm_idx > 0 && s_psram_audio_buf) {
                        size_t chunk_bytes = pcm_idx * sizeof(int16_t);
                        memcpy(s_psram_audio_buf + s_actual_bytes, pcm_chunk, chunk_bytes);
                        s_actual_bytes += chunk_bytes;
                    }
                }
            }

            adc_continuous_stop(adc_handle);
            s_task_running = false;
            s_is_recording = false;
            ESP_LOGI(TAG, "PSRAM Voice Recording Completed: %u bytes stored in 8MB PSRAM", (unsigned int)s_actual_bytes);
            if (s_sync_sem) {
                xSemaphoreGive(s_sync_sem);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t audio_recorder_init(const audio_recorder_config_t *config) {
    if (!config || config->sample_rate == 0 || config->record_seconds == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_sample_rate = config->sample_rate;
    s_total_target_bytes = s_sample_rate * 2 * config->record_seconds;

    // Allocate 160 KB directly in PSRAM
    s_psram_audio_buf = (uint8_t *)heap_caps_malloc(s_total_target_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_psram_audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for audio!", (unsigned int)s_total_target_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_sync_sem = xSemaphoreCreateBinary();

    adc_continuous_handle_cfg_t hdl_cfg = {
        .max_store_buf_size = 2048,
        .conv_frame_size = 512,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hdl_cfg, &adc_handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = s_sample_rate,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = ADC_CHANNEL,
        .unit = ADC_UNIT,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = &adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

    xTaskCreate(audio_record_task, "audio_rec_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "PSRAM Zero-Disk Audio Recorder Ready (%d sec capacity)", config->record_seconds);
    return ESP_OK;
}

esp_err_t audio_recorder_start(void) {
    if (s_is_recording || s_task_running) return ESP_ERR_INVALID_STATE;
    s_actual_bytes = 0;
    s_is_recording = true;
    return ESP_OK;
}

void audio_recorder_stop(void) {
    if (!s_is_recording && !s_task_running) return;
    s_is_recording = false;
    if (s_sync_sem) {
        xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(600));
    }
}

bool audio_recorder_is_active(void) {
    return s_is_recording || s_task_running;
}

uint8_t* audio_recorder_get_psram_buffer(size_t *out_len) {
    if (out_len) {
        *out_len = s_actual_bytes;
    }
    return s_psram_audio_buf;
}