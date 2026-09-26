# Module and API Reference

This reference documents public headers and active application-facing interfaces.

## `battery_driver`

```c
typedef struct {
    float voltage;
    int bars;
    bool is_charging;
    bool is_critical;
} battery_status_t;

esp_err_t battery_driver_init(void);
battery_status_t battery_get_status(void);
```

The driver monitors ADC2 channel 3 on GPIO 14 through a 100k/47k divider. `battery_get_status()` applies calibration/fallback conversion, 1.5-second read limiting, smoothing, bar thresholds, charging detection, and critical detection.

## `ota_manager`

```c
typedef struct {
    char version[16];
    char changelog[64];
    char binary_url[128];
    bool update_available;
} ota_info_t;

esp_err_t ota_check_update_info(const char *manifest_url, ota_info_t *out_info);
esp_err_t ota_perform_firmware_update(const char *firmware_url);
```

`ota_check_update_info()` performs HTTPS GET and parses JSON fields `version`, `notes`, and `firmware_url`. A version different from compiled `v1.2.0` sets `update_available`. `ota_perform_firmware_update()` uses `esp_https_ota()` and restarts after success.

## `audio_recorder`

```c
typedef struct {
    uint32_t sample_rate;
    uint8_t record_seconds;
} audio_recorder_config_t;

esp_err_t audio_recorder_init(const audio_recorder_config_t *config);
esp_err_t audio_recorder_start(void);
void audio_recorder_stop(void);
bool audio_recorder_is_active(void);
size_t audio_recorder_get_max_buffer_size(void);
const char* audio_recorder_get_sd_filepath(size_t *out_recorded_bytes);
```

The recorder samples ADC1 channel 3 / GPIO 4, converts 12-bit readings to clipped signed 16-bit samples, and writes `/sd/rec/voice.pcm`. Start rejects overlapping recordings; stop waits up to 600 ms for the task. The filepath function returns the fixed path and recorded byte count.

## `backend_client`

```c
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
esp_err_t backend_client_send_media_cmd(const char *cmd_str);
```

`backend_client_init()` stores the base URL and removes a trailing slash. `backend_client_send_audio_sd()` posts raw PCM to `/api/emo/chat`, parses `X-Emo-Reply` and `X-Emo-Emotion`, and streams HTTP 200 response bytes to the speaker. `backend_client_poll_action()` reads JSON field `action` from `/api/emo/poll_action`. `backend_client_send_media_cmd()` posts to `/api/bot/media_cmd?cmd=<command>`. `backend_client_free_result()` is a no-op.

## `display_driver`

Important public types:

```c
typedef enum {
    EMO_ANIM_IDLE, EMO_ANIM_BLINK, EMO_ANIM_LOOK_L, EMO_ANIM_LOOK_R,
    EMO_ANIM_HEARTS, EMO_ANIM_EATING, EMO_ANIM_HUNGRY, EMO_ANIM_SAD,
    EMO_ANIM_ANGRY, EMO_ANIM_SLEEPY, EMO_ANIM_LISTENING, EMO_ANIM_SPEAKING
} emo_face_anim_t;

typedef struct {
    int affection;
    int hunger;
    int energy;
    int total_chats;
    int total_pats;
    int total_feeds;
} emo_relationship_t;
```

Public operations include:

```c
esp_err_t display_controller_init(void);
esp_err_t sd_card_init(void);
void sd_card_deinit(void);
bool sd_card_file_exists(const char *filepath);
void display_precach_dock_icons_to_ram(void);
void display_draw_fast_ram_dock(bool is_expanded);
void display_draw_sd_eyes_sync(const char *folder_name, int frame_idx);
void display_draw_sd_raw(int x, int y, int w, int h, const char *filepath);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_rect_outline(int x, int y, int w, int h, uint16_t color);
void display_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale);
void display_clear_all(void);
void emo_draw_face(emo_face_anim_t anim, int tick);
void emo_draw_header_bar(const char *time_str, const char *date_str, const char *weather_str, bool wifi_ok, battery_status_t bat, int tick);
void emo_draw_sd_notification_popup(const char *app_name, const char *message);
emo_relationship_t* emo_metrics_get(void);
void emo_metrics_init(void);
void emo_metrics_save(void);
bool display_get_touch(int16_t *out_x, int16_t *out_y);
```

The actual header spells the relationship type `emo_relationship_t`; the representative declaration above should be read as `emo_relationship_t*`. The display uses RGB565 raw assets. Touch coordinates are accepted from raw values 200..3900 and mapped to 320x240. `emo_metrics_init()` and `emo_metrics_save()` are empty implementations.

## `speaker_driver`

```c
esp_err_t speaker_driver_init(int gpio_dout_num);
void speaker_driver_play_raw(const uint8_t *data, size_t size);
void speaker_play_sd_pcm(const char *filepath);
void speaker_driver_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void speaker_driver_play_base64(const char *base64_str);
```

The active driver uses ESP-IDF Sigma-Delta Modulation. Raw signed 16-bit PCM at 16 kHz is split into 512-byte packets and queued to a playback task pinned to core 1. Tone and base64 functions are currently empty.

## `touch_sensor`

```c
typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_HOLD_START,
    TOUCH_EVENT_HOLD_RELEASE,
    TOUCH_EVENT_PATTED,
    TOUCH_EVENT_TICKLE_LEFT,
    TOUCH_EVENT_TICKLE_RIGHT
} touch_gesture_t;

esp_err_t touch_sensor_init(int p1, int p2, int p3, int p4);
touch_gesture_t touch_sensor_update_and_poll(void);
bool touch_sensor_is_head_held(void);
```

This component provides configurable four-pad gesture detection. The current `main.c` directly samples the GPIOs in `touch_sensor_task()` instead of calling this API.

## `wifi_bridge`

```c
typedef enum {
    WIFI_MEDIA_PLAY_PAUSE = 0,
    WIFI_MEDIA_NEXT,
    WIFI_MEDIA_PREV,
    WIFI_MEDIA_VOL_UP,
    WIFI_MEDIA_VOL_DOWN
} wifi_media_cmd_t;

void wifi_bridge_set_gateway_ip(const char *gateway_ip);
esp_err_t wifi_bridge_send_media_cmd(wifi_media_cmd_t cmd);
```

It sends POST requests to `http://<gateway>:8080/media/<endpoint>`, defaulting to `192.168.43.1`. The active music UI uses the backend client instead.

## `groq_stt`

The component exposes `groq_stt_init()` and `groq_stt_transcribe()`, but the active application does not call them. Its transcription implementation writes placeholder text, so it is not the active speech-to-text pipeline.
