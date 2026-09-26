#include "speaker_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/sdm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SPEAKER_SDM";
#define SAMPLE_RATE_HZ 16000

#define AUDIO_CHUNK_SIZE 512
typedef struct {
    uint16_t len;
    uint8_t data[AUDIO_CHUNK_SIZE];
} audio_packet_t;

static sdm_channel_handle_t s_sdm_chan = NULL;
static bool s_initialized = false;
static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_speaker_task_handle = NULL;
static volatile bool s_is_playing_stream = false;

static void speaker_playback_task(void *pvParameters) {
    const int64_t sample_period_us = 1000000 / SAMPLE_RATE_HZ;
    audio_packet_t packet;

    while (1) {
        if (xQueueReceive(s_audio_queue, &packet, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_is_playing_stream = true;
            const int16_t *samples = (const int16_t *)packet.data;
            size_t num_samples = packet.len / 2;
            int64_t next_tick = esp_timer_get_time();

            for (size_t i = 0; i < num_samples; i++) {
                int32_t val = (int32_t)samples[i];
                int32_t duty = (val * 112) / 32768;
                if (duty > 112) duty = 112;
                if (duty < -112) duty = -112;

                sdm_channel_set_duty(s_sdm_chan, (int8_t)duty);
                next_tick += sample_period_us;

                while (esp_timer_get_time() < next_tick) {
                    // Precision hardware cadence
                }
            }
        } else {
            if (s_is_playing_stream) {
                sdm_channel_set_duty(s_sdm_chan, 0);
                s_is_playing_stream = false;
            }
        }
    }
}

esp_err_t speaker_driver_init(int gpio_dout_num) {
    if (s_initialized) return ESP_OK;

    sdm_config_t sdm_cfg = {
        .gpio_num = gpio_dout_num,
        .sample_rate_hz = 1000000,
        .clk_src = SDM_CLK_SRC_DEFAULT,
    };

    ESP_ERROR_CHECK(sdm_new_channel(&sdm_cfg, &s_sdm_chan));
    ESP_ERROR_CHECK(sdm_channel_enable(s_sdm_chan));
    ESP_ERROR_CHECK(sdm_channel_set_duty(s_sdm_chan, 0));

    // 48 Packets = ~24 KB buffer queue (smooth non-blocking stream)
    s_audio_queue = xQueueCreate(48, sizeof(audio_packet_t));
    xTaskCreatePinnedToCore(speaker_playback_task, "spk_play_task", 3072, NULL, 6, &s_speaker_task_handle, 1);

    s_initialized = true;
    ESP_LOGI(TAG, "Hardware Sigma-Delta Modulator active on GPIO %d (Deep Queue)", gpio_dout_num);
    return ESP_OK;
}

void speaker_driver_play_raw(const uint8_t *data, size_t size) {
    if (!s_initialized || !data || size == 0) return;

    size_t offset = 0;
    while (offset < size) {
        audio_packet_t packet;
        size_t chunk = size - offset;
        if (chunk > AUDIO_CHUNK_SIZE) chunk = AUDIO_CHUNK_SIZE;

        packet.len = (uint16_t)chunk;
        memcpy(packet.data, data + offset, chunk);

        // Send to queue with generous wait time
        xQueueSend(s_audio_queue, &packet, pdMS_TO_TICKS(500));
        offset += chunk;
    }
}

void speaker_play_sd_pcm(const char *filepath) {
    if (!filepath) return;
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    uint8_t pcm_buf[512];
    size_t bytes_read = 0;
    while ((bytes_read = fread(pcm_buf, 1, sizeof(pcm_buf), f)) > 0) {
        speaker_driver_play_raw(pcm_buf, bytes_read);
    }
    fclose(f);
}

void speaker_driver_play_tone(uint32_t freq_hz, uint32_t duration_ms) {}
void speaker_driver_play_base64(const char *base64_str) {}