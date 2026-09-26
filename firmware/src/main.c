#include "config.h"
#include "battery_driver.h"
#include "display_driver.h"
#include "backend_client.h"
#include "speaker_driver.h"
#include "audio_recorder.h"
#include "ota_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "DIVAAN_N16R8";

typedef enum {
    MODE_FACE_HOME = 0,
    MODE_ALARM_CLOCK,
    MODE_ALARM_RINGING,
    MODE_GREETING_POPUP,
    MODE_FOCUS_TIMER,
    MODE_GAME_TICTACTOE,
    MODE_GAME_QUICKTAP,
    MODE_MUSIC_PLAYER,
    MODE_SLEEPING,
    MODE_AI_LISTENING,
    MODE_AI_THINKING,
    MODE_AI_SPEAKING,
    MODE_ANIM_LOVE,
    MODE_ANIM_PIZZA,
    MODE_OTA_POPUP,
    MODE_OTA_UPDATING
} divaan_mode_t;

typedef enum {
    FOCUS_STATE_IDLE = 0,
    FOCUS_STATE_RUNNING,
    FOCUS_STATE_PAUSED
} focus_state_t;

typedef struct {
    int event_type; // 1: NOTIF, 2: SONG, 3: PIZZA, 4: LOVE, 5: OTA, 6: SLEEP, 7: SET_ALARM
    char app_or_title[48];
    char msg[64];
    int val1;
    int val2;
    int val3;
} ui_event_t;

typedef struct {
    int audio_type; // 1: Voice Upload, 2: Focus Chime, 3: Love, 4: Eat, 5: Pet Purr
} audio_req_t;

static QueueHandle_t s_ui_event_queue;
static QueueHandle_t s_audio_upload_queue;

static divaan_mode_t s_current_mode = MODE_FACE_HOME;
static divaan_mode_t s_previous_mode = MODE_FACE_HOME;
static bool s_mode_changed = true;
static bool s_wifi_connected = false;

static bool s_dock_expanded = false;
static int s_dock_rendered_state = -1;

// Alarm State (Persisted in Hardware NVS Flash)
static bool s_alarm_enabled = true;
static int s_alarm_hour = 7;
static int s_alarm_min = 30;
static bool s_alarm_editing = false;
static bool s_alarm_ringing = false;
static int s_alarm_last_triggered_min = -1;
static char s_greeting_msg[32] = "GOOD MORNING!";
static int64_t s_greeting_start_time = 0;

// OTA Information
static ota_info_t s_ota_info = {0};

// Focus Engine
static focus_state_t s_focus_state = FOCUS_STATE_IDLE;
static int s_focus_duration_mins = 25;
static int s_focus_remaining_sec = 25 * 60;
static int64_t s_focus_last_hw_timestamp = 0;

// Media & Games
static char s_active_song[64] = "No Track Playing";
static bool s_is_playing = false;

static int s_tap_target_x = 1;
static int s_tap_target_y = 1;
static int s_tap_score = 0;
static int64_t s_tap_last_move = 0;

static int s_ttt_board[3][3] = {0};
static int s_ttt_turn = 1;
static int s_ttt_winner = 0;

static bool s_showing_notif = false;
static int64_t s_notif_start_time = 0;
static int64_t s_action_anim_start = 0;
static int64_t s_last_user_activity = 0;

// Live Weather
static char s_weather_str[16] = "28 C";
static int64_t s_last_weather_fetch = 0;

static void update_live_weather(void) {
    if (!s_wifi_connected) return;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_weather_fetch < 1800000000LL && s_last_weather_fetch != 0) {
        return;
    }
    s_last_weather_fetch = now_us;

    char response_buf[256] = {0};
    esp_http_client_config_t cfg = {
        .url = "http://api.open-meteo.com/v1/forecast?latitude=11.0168&longitude=76.9558&current_weather=true",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client && esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int read_len = esp_http_client_read(client, response_buf, sizeof(response_buf) - 1);
        if (read_len > 0) {
            response_buf[read_len] = '\0';
            char *p = strstr(response_buf, "\"temperature\":");
            if (p) {
                float temp = 28.0f;
                if (sscanf(p + 14, "%f", &temp) == 1) {
                    snprintf(s_weather_str, sizeof(s_weather_str), "%d C", (int)roundf(temp));
                }
            }
        }
        esp_http_client_cleanup(client);
    }
}

// ----------------- Persistent Alarm Storage (Hardware NVS Flash) -----------------
static void load_alarm_settings(void) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        uint8_t hr = 7, min = 30, en = 1;
        nvs_get_u8(nvs_h, "alm_hr", &hr);
        nvs_get_u8(nvs_h, "alm_min", &min);
        nvs_get_u8(nvs_h, "alm_en", &en);
        s_alarm_hour = hr;
        s_alarm_min = min;
        s_alarm_enabled = (en == 1);
        nvs_close(nvs_h);
        ESP_LOGI(TAG, "Alarm loaded from NVS: %02d:%02d (%s)", s_alarm_hour, s_alarm_min, s_alarm_enabled ? "ON" : "OFF");
        return;
    }
    s_alarm_hour = 7;
    s_alarm_min = 30;
    s_alarm_enabled = true;
}

static void save_alarm_settings(void) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_u8(nvs_h, "alm_hr", (uint8_t)s_alarm_hour);
        nvs_set_u8(nvs_h, "alm_min", (uint8_t)s_alarm_min);
        nvs_set_u8(nvs_h, "alm_en", (uint8_t)(s_alarm_enabled ? 1 : 0));
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
        ESP_LOGI(TAG, "Alarm persisted to NVS: %02d:%02d", s_alarm_hour, s_alarm_min);
    }
}

// ----------------- Wi-Fi Subsystem -----------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        ESP_LOGI(TAG, "Wi-Fi Ready & IP Acquired");
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EMO_WIFI_SSID,
            .password = EMO_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void draw_restored_music_player(const char *track_title, bool is_playing, int tick) {
    display_fill_rect(0, 0, LCD_WIDTH, 28, EMO_COLOR_CARD_BG);
    display_draw_text(12, 8, "< HOME", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    display_draw_text(105, 8, "MEDIA CONTROLLER", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);

    display_fill_rect(16, 36, 288, 48, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(16, 36, 288, 48, EMO_COLOR_CYAN_GLOW);
    display_draw_text(26, 52, track_title, EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);

    for (int i = 0; i < 10; i++) {
        int height = 8;
        if (is_playing) {
            height = 10 + (int)((sin((tick * 0.35) + (i * 0.8)) + 1.0) * 22.0);
        }
        int col_x = 34 + (i * 26);
        int col_y = 145 - height;

        display_fill_rect(col_x, 90, 16, 55, EMO_COLOR_BG);
        display_fill_rect(col_x, col_y, 16, height, EMO_COLOR_CYAN_GLOW);
        display_fill_rect(col_x, col_y - 3, 16, 2, 0xFFFF);
    }

    display_fill_rect(24, 158, 76, 54, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(24, 158, 76, 54, EMO_COLOR_CYAN_GLOW);
    display_draw_text(46, 176, "|<<", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 2);

    display_fill_rect(116, 152, 88, 66, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(116, 152, 88, 66, EMO_COLOR_CYAN_GLOW);
    if (is_playing) {
        display_draw_text(146, 172, "||", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 3);
    } else {
        display_draw_text(150, 172, ">", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 3);
    }

    display_fill_rect(220, 158, 76, 54, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(220, 158, 76, 54, EMO_COLOR_CYAN_GLOW);
    display_draw_text(244, 176, ">>|", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 2);

    display_draw_text(70, 226, is_playing ? "PLAYING VIA PHONE" : "PLAYBACK PAUSED", EMO_COLOR_MUTED_TEXT, EMO_COLOR_BG, 1);
}

static void draw_home_dock(bool force_redraw) {
    int target_state = s_dock_expanded ? 1 : 0;
    if (!force_redraw && s_dock_rendered_state == target_state) {
        return;
    }
    s_dock_rendered_state = target_state;
    display_draw_fast_ram_dock(s_dock_expanded);
}

// ----------------- Unbeatable Minimax AI Engine -----------------
static void reset_ttt_game(void) {
    memset(s_ttt_board, 0, sizeof(s_ttt_board));
    s_ttt_turn = 1;
    s_ttt_winner = 0;
}

static int check_ttt_winner(void) {
    for (int i = 0; i < 3; i++) {
        if (s_ttt_board[i][0] != 0 && s_ttt_board[i][0] == s_ttt_board[i][1] && s_ttt_board[i][1] == s_ttt_board[i][2])
            return s_ttt_board[i][0];
        if (s_ttt_board[0][i] != 0 && s_ttt_board[0][i] == s_ttt_board[1][i] && s_ttt_board[1][i] == s_ttt_board[2][i])
            return s_ttt_board[0][i];
    }
    if (s_ttt_board[0][0] != 0 && s_ttt_board[0][0] == s_ttt_board[1][1] && s_ttt_board[1][1] == s_ttt_board[2][2])
        return s_ttt_board[0][0];
    if (s_ttt_board[0][2] != 0 && s_ttt_board[0][2] == s_ttt_board[1][1] && s_ttt_board[1][1] == s_ttt_board[2][0])
        return s_ttt_board[0][2];

    bool full = true;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (s_ttt_board[r][c] == 0) full = false;

    return full ? 3 : 0;
}

static int minimax(bool is_ai) {
    int score = check_ttt_winner();
    if (score == 2) return 10;
    if (score == 1) return -10;
    if (score == 3) return 0;

    if (is_ai) {
        int best = -1000;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                if (s_ttt_board[r][c] == 0) {
                    s_ttt_board[r][c] = 2;
                    int val = minimax(false);
                    s_ttt_board[r][c] = 0;
                    if (val > best) best = val;
                }
            }
        }
        return best;
    } else {
        int best = 1000;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                if (s_ttt_board[r][c] == 0) {
                    s_ttt_board[r][c] = 1;
                    int val = minimax(true);
                    s_ttt_board[r][c] = 0;
                    if (val < best) best = val;
                }
            }
        }
        return best;
    }
}

static void run_unbeatable_ttt_ai_move(void) {
    if (s_ttt_winner != 0) return;
    int best_val = -1000;
    int best_r = -1, best_c = -1;

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (s_ttt_board[r][c] == 0) {
                s_ttt_board[r][c] = 2;
                int move_val = minimax(false);
                s_ttt_board[r][c] = 0;
                if (move_val > best_val) {
                    best_val = move_val;
                    best_r = r;
                    best_c = c;
                }
            }
        }
    }

    if (best_r != -1 && best_c != -1) {
        s_ttt_board[best_r][best_c] = 2;
        s_ttt_winner = check_ttt_winner();
        s_ttt_turn = 1;
    }
}

static void draw_ttt_screen(void) {
    display_fill_rect(0, 0, LCD_WIDTH, 26, EMO_COLOR_CARD_BG);
    display_draw_text(12, 8, "< HOME", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    display_draw_text(100, 8, "TIC-TAC-TOE AI", 0x07E0, EMO_COLOR_CARD_BG, 1);

    int start_x = 75, start_y = 36, sz = 52;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = start_x + (c * (sz + 6));
            int by = start_y + (r * (sz + 6));
            display_fill_rect(bx, by, sz, sz, EMO_COLOR_CARD_BG);
            display_draw_rect_outline(bx, by, sz, sz, EMO_COLOR_CARD_BORDER);

            if (s_ttt_board[r][c] == 1) display_draw_text(bx + 18, by + 16, "X", 0xF800, EMO_COLOR_CARD_BG, 3);
            else if (s_ttt_board[r][c] == 2) display_draw_text(bx + 18, by + 16, "O", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 3);
        }
    }

    if (s_ttt_winner != 0) {
        display_fill_rect(40, 210, 240, 25, EMO_COLOR_CARD_BG);
        const char *w_str = (s_ttt_winner == 1) ? "YOU WIN! Tap replay" :
                            (s_ttt_winner == 2) ? "DIVAAN WINS! Tap replay" : "DRAW! Tap replay";
        display_draw_text(50, 216, w_str, 0xFFE0, EMO_COLOR_CARD_BG, 1);
    }
}

static void draw_quicktap_screen(void) {
    display_fill_rect(0, 0, LCD_WIDTH, 26, EMO_COLOR_CARD_BG);
    display_draw_text(12, 8, "< HOME", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    char score_str[32];
    snprintf(score_str, sizeof(score_str), "SCORE: %d", s_tap_score);
    display_draw_text(220, 8, score_str, 0x07E0, EMO_COLOR_CARD_BG, 1);

    int box_sz = 58, start_x = 70, start_y = 38;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = start_x + (c * (box_sz + 8));
            int by = start_y + (r * (box_sz + 6));

            if (r == s_tap_target_y && c == s_tap_target_x) {
                display_fill_rect(bx, by, box_sz, box_sz, EMO_COLOR_CYAN_GLOW);
                display_draw_text(bx + 16, by + 20, "TAP!", EMO_COLOR_BG, EMO_COLOR_CYAN_GLOW, 1);
            } else {
                display_fill_rect(bx, by, box_sz, box_sz, EMO_COLOR_CARD_BG);
                display_draw_rect_outline(bx, by, box_sz, box_sz, EMO_COLOR_CARD_BORDER);
            }
        }
    }
}

static void draw_alarm_screen(void) {
    display_fill_rect(0, 0, LCD_WIDTH, 26, EMO_COLOR_CARD_BG);
    display_draw_text(12, 8, "< HOME", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    display_draw_text(105, 8, "ALARM CONTROLLER", EMO_COLOR_MUTED_TEXT, EMO_COLOR_CARD_BG, 1);

    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d : %02d", s_alarm_hour, s_alarm_min);
    uint16_t time_border = s_alarm_editing ? 0xFFE0 : EMO_COLOR_CYAN_GLOW;
    display_fill_rect(60, 36, 200, 52, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(60, 36, 200, 52, time_border);
    display_draw_text(92, 48, time_str, EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 3);

    uint16_t btn_col = s_alarm_editing ? EMO_COLOR_CYAN_GLOW : EMO_COLOR_MUTED_TEXT;
    display_fill_rect(60, 96, 44, 38, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(60, 96, 44, 38, btn_col);
    display_draw_text(76, 106, "+", btn_col, EMO_COLOR_CARD_BG, 2);

    display_fill_rect(112, 96, 44, 38, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(112, 96, 44, 38, btn_col);
    display_draw_text(128, 106, "-", btn_col, EMO_COLOR_CARD_BG, 2);

    display_fill_rect(164, 96, 44, 38, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(164, 96, 44, 38, btn_col);
    display_draw_text(180, 106, "+", btn_col, EMO_COLOR_CARD_BG, 2);

    display_fill_rect(216, 96, 44, 38, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(216, 96, 44, 38, btn_col);
    display_draw_text(232, 106, "-", btn_col, EMO_COLOR_CARD_BG, 2);

    if (s_alarm_editing) {
        display_fill_rect(60, 146, 95, 42, 0x07E0);
        display_draw_text(85, 160, "SAVE", EMO_COLOR_BG, 0x07E0, 2);
    } else {
        display_fill_rect(60, 146, 95, 42, EMO_COLOR_CARD_BG);
        display_draw_rect_outline(60, 146, 95, 42, 0xFFE0);
        display_draw_text(85, 160, "EDIT", 0xFFE0, EMO_COLOR_CARD_BG, 2);
    }

    uint16_t state_col = s_alarm_enabled ? 0x07E0 : 0xF800;
    display_fill_rect(165, 146, 95, 42, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(165, 146, 95, 42, state_col);
    display_draw_text(180, 160, s_alarm_enabled ? "ARMED" : "OFF", state_col, EMO_COLOR_CARD_BG, 2);

    display_draw_text(68, 206, s_alarm_editing ? "TAP +/- TO CHANGE, THEN SAVE" : "TAP EDIT TO MODIFY TIME", EMO_COLOR_MUTED_TEXT, EMO_COLOR_BG, 1);
}

static void draw_alarm_ringing_screen(int tick) {
    uint16_t ring_col = (tick % 4 < 2) ? 0xF800 : 0xFFE0;
    display_fill_rect(20, 20, 280, 200, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(20, 20, 280, 200, ring_col);

    display_draw_text(60, 50, "ALARM RINGING!", ring_col, EMO_COLOR_CARD_BG, 2);

    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", s_alarm_hour, s_alarm_min);
    display_draw_text(100, 95, time_str, 0xFFFF, EMO_COLOR_CARD_BG, 3);

    display_fill_rect(40, 150, 240, 45, ring_col);
    display_draw_text(58, 164, "TAP SCREEN TO STOP", EMO_COLOR_BG, ring_col, 2);
}

static void draw_greeting_screen(void) {
    display_fill_rect(20, 30, 280, 180, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(20, 30, 280, 180, 0x07E0);

    display_draw_text(60, 70, "HAVE A WONDERFUL DAY!", 0xFFE0, EMO_COLOR_CARD_BG, 1);
    display_draw_text(40, 110, s_greeting_msg, 0x07E0, EMO_COLOR_CARD_BG, 2);
    display_draw_text(80, 160, "DIVAAN IS READY", EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
}

static void draw_focus_screen(int tick) {
    display_fill_rect(0, 0, LCD_WIDTH, 26, EMO_COLOR_CARD_BG);
    if (s_focus_state == FOCUS_STATE_RUNNING) display_draw_text(12, 8, "[LOCKED IN]", 0xF800, EMO_COLOR_CARD_BG, 1);
    else display_draw_text(12, 8, "< HOME", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    display_draw_text(105, 8, "DEEP FOCUS MODE", 0xFFE0, EMO_COLOR_CARD_BG, 1);

    int mins = s_focus_remaining_sec / 60;
    int secs = s_focus_remaining_sec % 60;
    char time_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", mins, secs);

    display_fill_rect(40, 36, 240, 54, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(40, 36, 240, 54, 0xFFE0);
    display_draw_text(98, 46, time_buf, 0xFFE0, EMO_COLOR_CARD_BG, 4);

    int total_sec = s_focus_duration_mins * 60;
    int bar_w = (total_sec > 0) ? (s_focus_remaining_sec * 240) / total_sec : 0;
    display_fill_rect(40, 96, 240, 6, EMO_COLOR_CARD_BORDER);
    display_fill_rect(40, 96, bar_w, 6, 0xFFE0);

    if (s_focus_state == FOCUS_STATE_IDLE) {
        display_fill_rect(40, 110, 115, 36, EMO_COLOR_CARD_BG);
        display_draw_rect_outline(40, 110, 115, 36, EMO_COLOR_CARD_BORDER);
        display_draw_text(60, 122, "- 5 MIN", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);

        display_fill_rect(165, 110, 115, 36, EMO_COLOR_CARD_BG);
        display_draw_rect_outline(165, 110, 115, 36, EMO_COLOR_CARD_BORDER);
        display_draw_text(185, 122, "+ 5 MIN", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
    }

    const char *action_lbl = (s_focus_state == FOCUS_STATE_IDLE) ? "START" :
                             (s_focus_state == FOCUS_STATE_RUNNING) ? "PAUSE" : "RESUME";
    uint16_t action_color = (s_focus_state == FOCUS_STATE_RUNNING) ? 0xF800 : 0x07E0;

    display_fill_rect(40, 154, 115, 42, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(40, 154, 115, 42, action_color);
    display_draw_text(70, 168, action_lbl, action_color, EMO_COLOR_CARD_BG, 2);

    display_fill_rect(165, 154, 115, 42, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(165, 154, 115, 42, EMO_COLOR_MUTED_TEXT);
    display_draw_text(198, 168, "RESET", EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 2);
}

static void draw_ota_popup_screen(void) {
    display_fill_rect(16, 20, 288, 200, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(16, 20, 288, 200, EMO_COLOR_CYAN_GLOW);

    display_draw_text(50, 36, "NEW FIRMWARE UPDATE!", 0xFFE0, EMO_COLOR_CARD_BG, 1);

    char ver_str[32];
    snprintf(ver_str, sizeof(ver_str), "VERSION: %s", s_ota_info.version);
    display_draw_text(30, 65, ver_str, EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 2);

    display_draw_text(30, 95, "CHANGELOG:", EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
    display_draw_text(30, 115, s_ota_info.changelog, EMO_COLOR_MUTED_TEXT, EMO_COLOR_CARD_BG, 1);

    display_fill_rect(30, 150, 125, 45, 0x07E0);
    display_draw_text(42, 165, "UPDATE NOW", EMO_COLOR_BG, 0x07E0, 1);

    display_fill_rect(165, 150, 125, 45, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(165, 150, 125, 45, EMO_COLOR_MUTED_TEXT);
    display_draw_text(205, 165, "LATER", EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
}

static void draw_ota_updating_screen(int tick) {
    display_fill_rect(20, 30, 280, 180, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(20, 30, 280, 180, 0xFFE0);

    display_draw_text(50, 60, "DOWNLOADING FIRMWARE", 0xFFE0, EMO_COLOR_CARD_BG, 1);
    display_draw_text(40, 95, "DO NOT TURN OFF BOT!", 0xF800, EMO_COLOR_CARD_BG, 1);

    int bar_w = (tick * 12) % 240;
    display_fill_rect(40, 135, 240, 14, EMO_COLOR_CARD_BORDER);
    display_fill_rect(40, 135, bar_w, 14, EMO_COLOR_CYAN_GLOW);
}

static void ota_download_task(void *pv) {
    ESP_LOGI(TAG, "Dedicated OTA Task executing...");
    ota_perform_firmware_update(s_ota_info.binary_url);
    vTaskDelete(NULL);
}

static void audio_upload_task(void *pv) {
    audio_req_t req;
    while (1) {
        if (xQueueReceive(s_audio_upload_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (req.audio_type == 1) {
                s_current_mode = MODE_AI_THINKING;
                s_mode_changed = true;
                ESP_LOGI(TAG, "Streaming recorded voice from 8MB PSRAM to Gemini...");

                size_t pcm_len = 0;
                uint8_t *psram_buf = audio_recorder_get_psram_buffer(&pcm_len);

                emo_api_result_t api_res;
                s_current_mode = MODE_AI_SPEAKING;
                s_mode_changed = true;

                if (psram_buf && pcm_len > 0) {
                    backend_client_send_audio_psram(psram_buf, pcm_len, &api_res);
                }

                // Smoothly release AI lock and return Home
                s_current_mode = MODE_FACE_HOME;
                s_mode_changed = true;
                s_last_user_activity = esp_timer_get_time();

                emo_relationship_t *m = emo_metrics_get();
                m->affection = (m->affection + 2 > 100) ? 100 : m->affection + 2;
                m->total_chats++;
            } else if (req.audio_type >= 2 && req.audio_type <= 5) {
                int16_t tone_buf[256];
                float base_f = (req.audio_type == 2) ? 523.25f : 
                               ((req.audio_type == 3) ? 880.0f : 
                               ((req.audio_type == 4) ? 440.0f : 660.0f)); // 5: Purr
                for (int i = 0; i < 256; i++) {
                    float t = (float)i / 16000.0f;
                    tone_buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * base_f * t) * 11000.0f);
                }
                for (int k = 0; k < 8; k++) {
                    speaker_driver_play_raw((const uint8_t *)tone_buf, sizeof(tone_buf));
                }
            }
        }
    }
}

// ----------------- 3-Touch Sensor Handler: Dual-Petting & Anti-Glitch State Machine -----------------
static void touch_sensor_task(void *pv) {
    gpio_set_direction(PIN_NUM_TOUCH_2, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_NUM_TOUCH_3, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_NUM_TOUCH_4, GPIO_MODE_INPUT);

    int64_t touch2_press_start = 0;
    int64_t pet_start_time = 0;
    bool is_recording = false;
    bool pet_triggered = false;
    bool t3_latched = false;
    bool t4_latched = false;

    while (1) {
        int t2 = gpio_get_level(PIN_NUM_TOUCH_2);
        int t3 = gpio_get_level(PIN_NUM_TOUCH_3);
        int t4 = gpio_get_level(PIN_NUM_TOUCH_4);

        // Completely ignore capacitive pads if AI pipeline is engaged
        if (s_current_mode == MODE_AI_LISTENING || s_current_mode == MODE_AI_THINKING || s_current_mode == MODE_AI_SPEAKING) {
            touch2_press_start = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 1. Dual-Pad Petting Gesture (Touch 2 + Touch 3 held simultaneously)
        if (t2 == 1 && t3 == 1) {
            s_last_user_activity = esp_timer_get_time();
            if (pet_start_time == 0) {
                pet_start_time = esp_timer_get_time();
            } else if (!pet_triggered && (esp_timer_get_time() - pet_start_time >= 250000)) {
                pet_triggered = true;
                emo_relationship_t *m = emo_metrics_get();
                m->affection = (m->affection + 4 > 100) ? 100 : m->affection + 4;
                m->total_pats++;

                s_current_mode = MODE_ANIM_LOVE;
                s_action_anim_start = esp_timer_get_time();
                s_mode_changed = true;

                audio_req_t req = { .audio_type = 5 }; // Purr tone
                xQueueSend(s_audio_upload_queue, &req, 0);
                ESP_LOGI(TAG, "Dual Touch Petting: Affection raised to %d", m->affection);
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else {
            if (pet_triggered) {
                pet_triggered = false;
                pet_start_time = 0;
            }
        }

        // 2. Touch 2: Push-to-Talk (Hold >= 350ms) OR Short Tap Focus Toggle
        if (t2 == 1) {
            s_last_user_activity = esp_timer_get_time();
            if (touch2_press_start == 0) {
                touch2_press_start = esp_timer_get_time();
            } else if (!is_recording && (esp_timer_get_time() - touch2_press_start >= 350000)) {
                if (s_current_mode == MODE_FACE_HOME && s_focus_state != FOCUS_STATE_RUNNING) {
                    is_recording = true;
                    s_current_mode = MODE_AI_LISTENING;
                    s_mode_changed = true;
                    audio_recorder_start();
                    ESP_LOGI(TAG, "Touch 2 Hold: Recording voice to PSRAM...");
                }
            }
        } else {
            if (touch2_press_start > 0) {
                int64_t duration_us = esp_timer_get_time() - touch2_press_start;
                touch2_press_start = 0;

                if (is_recording) {
                    is_recording = false;
                    audio_recorder_stop();
                    audio_req_t req = { .audio_type = 1 };
                    xQueueSend(s_audio_upload_queue, &req, 0);
                } else if (duration_us >= 50000 && duration_us < 350000) {
                    if (s_alarm_ringing) {
                        s_alarm_ringing = false;
                    } else if (s_focus_state != FOCUS_STATE_RUNNING && s_current_mode != MODE_ALARM_CLOCK) {
                        // Safe focus toggle
                        s_current_mode = (s_current_mode == MODE_FOCUS_TIMER) ? MODE_FACE_HOME : MODE_FOCUS_TIMER;
                        s_mode_changed = true;
                    }
                }
            }
        }

        // 3. Touch 3: Anti-Glitch Debounced Cycle (Only on explicit rising edge)
        if (t3 == 1) {
            if (!t3_latched && s_focus_state != FOCUS_STATE_RUNNING && !is_recording) {
                t3_latched = true;
                s_last_user_activity = esp_timer_get_time();
                s_current_mode = (s_current_mode + 1) % 6;
                s_mode_changed = true;
            }
        } else {
            t3_latched = false;
        }

        // 4. Touch 4: Anti-Glitch Debounced Sleep Toggle
        if (t4 == 1) {
            if (!t4_latched && s_focus_state != FOCUS_STATE_RUNNING && !is_recording) {
                t4_latched = true;
                s_last_user_activity = esp_timer_get_time();
                s_current_mode = (s_current_mode == MODE_SLEEPING) ? MODE_FACE_HOME : MODE_SLEEPING;
                s_mode_changed = true;
            }
        } else {
            t4_latched = false;
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

// ----------------- UI Render Loop: With Touch Lockout in AI Modes -----------------
static void ui_render_task(void *pv) {
    int tick = 0;
    time_t now;
    struct tm timeinfo;
    s_last_user_activity = esp_timer_get_time();

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        char time_buf[16], date_buf[16];
        strftime(time_buf, sizeof(time_buf), "%I:%M %p", &timeinfo);
        strftime(date_buf, sizeof(date_buf), "%a, %b %d", &timeinfo);

        battery_status_t bat_status = battery_get_status();
        int64_t current_hw_time = esp_timer_get_time();

        // Prevent idle sleep in active modes
        if (s_current_mode == MODE_MUSIC_PLAYER || s_is_playing || s_current_mode == MODE_FOCUS_TIMER || 
            s_current_mode == MODE_ALARM_CLOCK || s_current_mode == MODE_AI_LISTENING || 
            s_current_mode == MODE_AI_THINKING || s_current_mode == MODE_AI_SPEAKING) {
            s_last_user_activity = current_hw_time;
        }

        if (s_current_mode == MODE_FACE_HOME && s_focus_state != FOCUS_STATE_RUNNING && !s_is_playing) {
            if (current_hw_time - s_last_user_activity > 1800000000LL) {
                s_current_mode = MODE_SLEEPING;
                s_mode_changed = true;
            }
        }

        if (s_focus_state == FOCUS_STATE_RUNNING) {
            if (s_focus_last_hw_timestamp == 0) {
                s_focus_last_hw_timestamp = current_hw_time;
            } else {
                int64_t elapsed_us = current_hw_time - s_focus_last_hw_timestamp;
                if (elapsed_us >= 1000000) {
                    int elapsed_sec = (int)(elapsed_us / 1000000);
                    s_focus_remaining_sec -= elapsed_sec;
                    s_focus_last_hw_timestamp += (int64_t)elapsed_sec * 1000000;

                    if (s_focus_remaining_sec <= 0) {
                        s_focus_state = FOCUS_STATE_IDLE;
                        s_focus_remaining_sec = s_focus_duration_mins * 60;
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                        audio_req_t req = { .audio_type = 2 };
                        xQueueSend(s_audio_upload_queue, &req, 0);
                    }
                }
            }
        } else {
            s_focus_last_hw_timestamp = current_hw_time;
        }

        if (s_alarm_enabled && timeinfo.tm_hour == s_alarm_hour && timeinfo.tm_min == s_alarm_min) {
            if (s_alarm_last_triggered_min != timeinfo.tm_min) {
                s_alarm_ringing = true;
                s_alarm_last_triggered_min = timeinfo.tm_min;
                s_current_mode = MODE_ALARM_RINGING;
                s_mode_changed = true;
            }
        }

        if (s_alarm_ringing) {
            int16_t tone_buf[256];
            for (int i = 0; i < 256; i++) {
                float t = (float)i / 16000.0f;
                tone_buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * 987.77f * t) * 14000.0f);
            }
            for (int k = 0; k < 4; k++) {
                speaker_driver_play_raw((const uint8_t *)tone_buf, sizeof(tone_buf));
            }
        }

        ui_event_t ev;
        if (xQueueReceive(s_ui_event_queue, &ev, 0) == pdTRUE) {
            s_last_user_activity = esp_timer_get_time();
            if (ev.event_type == 1 && s_focus_state != FOCUS_STATE_RUNNING && s_current_mode != MODE_SLEEPING) {
                s_showing_notif = true;
                s_notif_start_time = esp_timer_get_time();
                emo_draw_sd_notification_popup(ev.app_or_title, ev.msg);
            } else if (ev.event_type == 3) {
                emo_relationship_t *m = emo_metrics_get();
                m->hunger = (m->hunger + 25 > 100) ? 100 : m->hunger + 25;
                m->affection = (m->affection + 5 > 100) ? 100 : m->affection + 5;
                m->total_feeds++;
                s_current_mode = MODE_ANIM_PIZZA;
                s_action_anim_start = esp_timer_get_time();
                s_mode_changed = true;
                audio_req_t req = { .audio_type = 4 };
                xQueueSend(s_audio_upload_queue, &req, 0);
            } else if (ev.event_type == 4) {
                emo_relationship_t *m = emo_metrics_get();
                m->affection = (m->affection + 10 > 100) ? 100 : m->affection + 10;
                m->total_pats++;
                s_current_mode = MODE_ANIM_LOVE;
                s_action_anim_start = esp_timer_get_time();
                s_mode_changed = true;
                audio_req_t req = { .audio_type = 3 };
                xQueueSend(s_audio_upload_queue, &req, 0);
            } else if (ev.event_type == 5) {
                s_current_mode = MODE_OTA_POPUP;
                s_mode_changed = true;
            } else if (ev.event_type == 6) {
                s_current_mode = MODE_SLEEPING;
                s_mode_changed = true;
            } else if (ev.event_type == 7) {
                s_alarm_hour = ev.val1;
                s_alarm_min = ev.val2;
                s_alarm_enabled = (ev.val3 == 1);
                save_alarm_settings();
                s_mode_changed = true;
            }
        }

        if (s_current_mode == MODE_ANIM_PIZZA || s_current_mode == MODE_ANIM_LOVE) {
            if (esp_timer_get_time() - s_action_anim_start > 3000000LL) {
                s_current_mode = MODE_FACE_HOME;
                s_mode_changed = true;
            }
        }

        if (s_current_mode == MODE_GREETING_POPUP) {
            if (esp_timer_get_time() - s_greeting_start_time > 3500000LL) {
                s_current_mode = MODE_FACE_HOME;
                s_mode_changed = true;
            }
        }

        if (s_showing_notif) {
            if (esp_timer_get_time() - s_notif_start_time > 2500000) {
                s_showing_notif = false;
                s_mode_changed = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        if (s_mode_changed || s_current_mode != s_previous_mode) {
            display_clear_all();
            s_previous_mode = s_current_mode;
            s_mode_changed = false;
            s_dock_rendered_state = -1;
        }

        // State Machine Switch
        switch (s_current_mode) {
            // AI states: Absolute Lockout from screen touch
            case MODE_AI_LISTENING:
                emo_draw_face(EMO_ANIM_LISTENING, tick);
                break;

            case MODE_AI_THINKING:
                emo_draw_face(EMO_ANIM_BLINK, tick);
                break;

            case MODE_AI_SPEAKING:
                emo_draw_face(EMO_ANIM_SPEAKING, tick);
                break;

            case MODE_SLEEPING:
                emo_draw_face(EMO_ANIM_SLEEPY, tick);
                break;

            case MODE_ANIM_LOVE:
                emo_draw_face(EMO_ANIM_HEARTS, tick);
                break;

            case MODE_ANIM_PIZZA:
                emo_draw_face(EMO_ANIM_EATING, tick);
                break;

            case MODE_OTA_POPUP: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (ty >= 150 && ty <= 195) {
                        if (tx >= 30 && tx <= 155) {
                            s_current_mode = MODE_OTA_UPDATING;
                            s_mode_changed = true;
                            xTaskCreatePinnedToCore(ota_download_task, "ota_worker", 10240, NULL, 5, NULL, 0);
                        } else if (tx >= 165 && tx <= 290) {
                            s_current_mode = MODE_FACE_HOME;
                            s_mode_changed = true;
                        }
                    }
                }
                draw_ota_popup_screen();
                break;
            }

            case MODE_OTA_UPDATING:
                draw_ota_updating_screen(tick);
                break;

            case MODE_ALARM_RINGING: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_alarm_ringing = false;
                    s_last_user_activity = esp_timer_get_time();

                    if (timeinfo.tm_hour >= 4 && timeinfo.tm_hour < 12) {
                        snprintf(s_greeting_msg, sizeof(s_greeting_msg), "GOOD MORNING!");
                    } else if (timeinfo.tm_hour >= 12 && timeinfo.tm_hour < 17) {
                        snprintf(s_greeting_msg, sizeof(s_greeting_msg), "GOOD AFTERNOON!");
                    } else if (timeinfo.tm_hour >= 17 && timeinfo.tm_hour < 21) {
                        snprintf(s_greeting_msg, sizeof(s_greeting_msg), "GOOD EVENING!");
                    } else {
                        snprintf(s_greeting_msg, sizeof(s_greeting_msg), "GOOD NIGHT!");
                    }

                    s_greeting_start_time = esp_timer_get_time();
                    s_current_mode = MODE_GREETING_POPUP;
                    s_mode_changed = true;
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                draw_alarm_ringing_screen(tick);
                break;
            }

            case MODE_GREETING_POPUP:
                draw_greeting_screen();
                break;

            case MODE_ALARM_CLOCK: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (ty <= 35 && tx <= 90) {
                        s_alarm_editing = false;
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                    } else if (ty >= 96 && ty <= 134 && s_alarm_editing) {
                        if (tx >= 60 && tx <= 104) { s_alarm_hour = (s_alarm_hour + 1) % 24; }
                        else if (tx >= 112 && tx <= 156) { s_alarm_hour = (s_alarm_hour + 23) % 24; }
                        else if (tx >= 164 && tx <= 208) { s_alarm_min = (s_alarm_min + 5) % 60; }
                        else if (tx >= 216 && tx <= 260) { s_alarm_min = (s_alarm_min + 55) % 60; }
                        vTaskDelay(pdMS_TO_TICKS(150));
                    } else if (ty >= 146 && ty <= 188) {
                        if (tx >= 60 && tx <= 155) {
                            s_alarm_editing = !s_alarm_editing;
                            if (!s_alarm_editing) {
                                save_alarm_settings();
                            }
                            vTaskDelay(pdMS_TO_TICKS(200));
                        } else if (tx >= 165 && tx <= 260) {
                            s_alarm_enabled = !s_alarm_enabled;
                            save_alarm_settings();
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    }
                }
                draw_alarm_screen();
                break;
            }

            case MODE_FOCUS_TIMER: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (s_focus_state != FOCUS_STATE_RUNNING && ty <= 35 && tx <= 90) {
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                    } else if (s_focus_state == FOCUS_STATE_IDLE && ty >= 110 && ty <= 146) {
                        if (tx >= 40 && tx <= 155 && s_focus_duration_mins > 5) {
                            s_focus_duration_mins -= 5;
                            s_focus_remaining_sec = s_focus_duration_mins * 60;
                        } else if (tx >= 165 && tx <= 280 && s_focus_duration_mins < 90) {
                            s_focus_duration_mins += 5;
                            s_focus_remaining_sec = s_focus_duration_mins * 60;
                        }
                        vTaskDelay(pdMS_TO_TICKS(180));
                    } else if (ty >= 154 && ty <= 196) {
                        if (tx >= 40 && tx <= 155) {
                            if (s_focus_state == FOCUS_STATE_IDLE) {
                                s_focus_state = FOCUS_STATE_RUNNING;
                                s_focus_last_hw_timestamp = esp_timer_get_time();
                            } else if (s_focus_state == FOCUS_STATE_RUNNING) {
                                s_focus_state = FOCUS_STATE_PAUSED;
                            } else if (s_focus_state == FOCUS_STATE_PAUSED) {
                                s_focus_state = FOCUS_STATE_RUNNING;
                                s_focus_last_hw_timestamp = esp_timer_get_time();
                            }
                            vTaskDelay(pdMS_TO_TICKS(200));
                        } else if (tx >= 165 && tx <= 280) {
                            s_focus_state = FOCUS_STATE_IDLE;
                            s_focus_remaining_sec = s_focus_duration_mins * 60;
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    }
                }
                draw_focus_screen(tick);
                break;
            }

            case MODE_GAME_TICTACTOE: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (ty <= 35 && tx <= 90) {
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                    } else if (s_ttt_winner != 0) {
                        reset_ttt_game();
                        display_clear_all();
                        vTaskDelay(pdMS_TO_TICKS(250));
                    } else if (s_ttt_turn == 1) {
                        int start_x = 75, start_y = 36, sz = 52;
                        for (int r = 0; r < 3; r++) {
                            for (int c = 0; c < 3; c++) {
                                int bx = start_x + (c * (sz + 6));
                                int by = start_y + (r * (sz + 6));
                                if (tx >= bx && tx <= bx + sz && ty >= by && ty <= by + sz && s_ttt_board[r][c] == 0) {
                                    s_ttt_board[r][c] = 1;
                                    s_ttt_winner = check_ttt_winner();
                                    if (s_ttt_winner == 0) {
                                        s_ttt_turn = 2;
                                        run_unbeatable_ttt_ai_move();
                                    }
                                    vTaskDelay(pdMS_TO_TICKS(200));
                                }
                            }
                        }
                    }
                }
                draw_ttt_screen();
                break;
            }

            case MODE_GAME_QUICKTAP: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (ty <= 35 && tx <= 90) {
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                    } else {
                        int start_x = 70 + (s_tap_target_x * 66);
                        int start_y = 38 + (s_tap_target_y * 64);
                        if (tx >= start_x && tx <= start_x + 58 && ty >= start_y && ty <= start_y + 58) {
                            s_tap_score += 10;
                            s_tap_target_x = (s_tap_target_x + 1) % 3;
                            s_tap_target_y = (s_tap_target_y + 2) % 3;
                            draw_quicktap_screen();
                        }
                    }
                }
                if (esp_timer_get_time() - s_tap_last_move > 1500000) {
                    s_tap_target_x = (s_tap_target_x + 1) % 3;
                    s_tap_target_y = (s_tap_target_y + 1) % 3;
                    s_tap_last_move = esp_timer_get_time();
                    draw_quicktap_screen();
                }
                break;
            }

            case MODE_MUSIC_PLAYER: {
                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (ty <= 35 && tx <= 90) {
                        s_current_mode = MODE_FACE_HOME;
                        s_mode_changed = true;
                    } else if (ty >= 150 && ty <= 220) {
                        if (tx >= 24 && tx <= 100) {
                            backend_client_send_media_cmd("PREV");
                            vTaskDelay(pdMS_TO_TICKS(250));
                        } else if (tx >= 116 && tx <= 204) {
                            s_is_playing = !s_is_playing;
                            backend_client_send_media_cmd("PLAY_PAUSE");
                            vTaskDelay(pdMS_TO_TICKS(250));
                        } else if (tx >= 220 && tx <= 296) {
                            backend_client_send_media_cmd("NEXT");
                            vTaskDelay(pdMS_TO_TICKS(250));
                        }
                    }
                }
                draw_restored_music_player(s_active_song, s_is_playing, tick);
                break;
            }

            case MODE_FACE_HOME:
            default: {
                emo_draw_header_bar(time_buf, date_buf, s_weather_str, s_wifi_connected, bat_status, tick);

                emo_relationship_t *m = emo_metrics_get();
                emo_face_anim_t anim = EMO_ANIM_IDLE;

                if (bat_status.is_critical && (tick / 4) % 2 == 0) {
                    anim = EMO_ANIM_SAD;
                } else if (m->hunger < 25) {
                    anim = EMO_ANIM_HUNGRY;
                } else if (m->affection < 25) {
                    anim = EMO_ANIM_SAD;
                } else if (m->affection >= 90) {
                    anim = EMO_ANIM_HEARTS;
                }

                emo_draw_face(anim, tick);

                int16_t tx = 0, ty = 0;
                if (display_get_touch(&tx, &ty)) {
                    s_last_user_activity = esp_timer_get_time();
                    if (!s_dock_expanded) {
                        if (tx >= 120 && tx <= 200 && ty >= 200) {
                            s_dock_expanded = true;
                            draw_home_dock(true);
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    } else {
                        if (ty >= 180 && ty <= 238) {
                            if (tx >= 16 && tx <= 64) { s_current_mode = MODE_ALARM_CLOCK; s_mode_changed = true; }
                            else if (tx >= 74 && tx <= 122) { s_current_mode = MODE_FOCUS_TIMER; s_mode_changed = true; }
                            else if (tx >= 132 && tx <= 180) { reset_ttt_game(); s_current_mode = MODE_GAME_TICTACTOE; s_mode_changed = true; }
                            else if (tx >= 190 && tx <= 238) { s_current_mode = MODE_GAME_QUICKTAP; s_mode_changed = true; }
                            else if (tx >= 248 && tx <= 296) { s_current_mode = MODE_MUSIC_PLAYER; s_mode_changed = true; }
                            s_dock_expanded = false;
                            draw_home_dock(true);
                            vTaskDelay(pdMS_TO_TICKS(200));
                        } else {
                            s_dock_expanded = false;
                            draw_home_dock(true);
                        }
                    }
                } else {
                    draw_home_dock(false);
                }
                break;
            }
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ----------------- Network Task -----------------
static void network_io_task(void *pv) {
    int64_t last_telemetry_time = 0;

    while (1) {
        if (s_wifi_connected && s_current_mode != MODE_OTA_UPDATING) {
            update_live_weather();

            int64_t now_us = esp_timer_get_time();
            if (now_us - last_telemetry_time > 10000000LL) {
                last_telemetry_time = now_us;
                battery_status_t bat = battery_get_status();
                emo_relationship_t *m = emo_metrics_get();

                char payload[128];
                snprintf(payload, sizeof(payload), 
                         "{\"bars\":%d,\"is_charging\":%s,\"affection\":%d,\"hunger\":%d,\"energy\":%d}",
                         bat.bars, bat.is_charging ? "true" : "false", m->affection, m->hunger, m->energy);

                esp_http_client_config_t cfg = {
                    .url = "https://divaan-backend.onrender.com/api/bot/sync_telemetry",
                    .method = HTTP_METHOD_POST,
                    .timeout_ms = 4000,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                };
                esp_http_client_handle_t client = esp_http_client_init(&cfg);
                if (client) {
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_open(client, strlen(payload));
                    esp_http_client_write(client, payload, strlen(payload));
                    esp_http_client_cleanup(client);
                }
            }

            char action[128] = {0};
            if (backend_client_poll_action(action, sizeof(action)) == ESP_OK && strlen(action) > 0 && strcmp(action, "NONE") != 0) {
                if (strncmp(action, "SONG:", 5) == 0) {
                    strncpy(s_active_song, action + 5, sizeof(s_active_song) - 1);
                    s_is_playing = true;
                } else if (strncmp(action, "NOTIF:", 6) == 0) {
                    if (s_focus_state != FOCUS_STATE_RUNNING) {
                        char app[32] = {0}, msg[64] = {0};
                        char *start = strchr(action, '[');
                        char *end = strchr(action, ']');
                        if (start && end && end > start) {
                            strncpy(app, start + 1, end - start - 1);
                            strncpy(msg, end + 2, sizeof(msg) - 1);

                            ui_event_t ev = { .event_type = 1 };
                            strncpy(ev.app_or_title, app, sizeof(ev.app_or_title) - 1);
                            strncpy(ev.msg, msg, sizeof(ev.msg) - 1);
                            xQueueSend(s_ui_event_queue, &ev, 0);
                        }
                    }
                } else if (strcmp(action, "FEED_PIZZA") == 0) {
                    ui_event_t ev = { .event_type = 3 };
                    xQueueSend(s_ui_event_queue, &ev, 0);
                } else if (strcmp(action, "PET_LOVE") == 0) {
                    ui_event_t ev = { .event_type = 4 };
                    xQueueSend(s_ui_event_queue, &ev, 0);
                } else if (strcmp(action, "FORCE_SLEEP") == 0) {
                    ui_event_t ev = { .event_type = 6 };
                    xQueueSend(s_ui_event_queue, &ev, 0);
                } else if (strncmp(action, "SET_ALARM:", 10) == 0) {
                    int hr = 7, min = 30, en = 1;
                    if (sscanf(action + 10, "%d:%d:%d", &hr, &min, &en) == 3) {
                        ui_event_t ev = { .event_type = 7, .val1 = hr, .val2 = min, .val3 = en };
                        xQueueSend(s_ui_event_queue, &ev, 0);
                    }
                } else if (strcmp(action, "CHECK_OTA") == 0) {
                    ESP_LOGI(TAG, "Received CHECK_OTA from backend! Querying manifest...");
                    if (ota_check_update_info("https://divaan-backend.onrender.com/api/device/manifest.json", &s_ota_info) == ESP_OK) {
                        if (s_ota_info.update_available) {
                            ui_event_t ev = { .event_type = 5 };
                            xQueueSend(s_ui_event_queue, &ev, 0);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting Divaan Native Engine (ESP32-S3-N16R8 Production)...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_ui_event_queue = xQueueCreate(8, sizeof(ui_event_t));
    s_audio_upload_queue = xQueueCreate(4, sizeof(audio_req_t));

    display_controller_init();
    load_alarm_settings();
    battery_driver_init();
    speaker_driver_init(PIN_NUM_SPEAKER_PDM);

    audio_recorder_config_t rec_cfg = {
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .record_seconds = AUDIO_MAX_RECORD_SEC,
    };
    audio_recorder_init(&rec_cfg);

    wifi_init_sta();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    setenv("TZ", "IST-5:30", 1);
    tzset();

    backend_client_init(EMO_BACKEND_BASE_URL);

    xTaskCreatePinnedToCore(audio_upload_task, "audio_worker", 10240, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(touch_sensor_task, "touch_task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(ui_render_task, "ui_task", 10240, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(network_io_task, "net_task", 10240, NULL, 4, NULL, 0);
}