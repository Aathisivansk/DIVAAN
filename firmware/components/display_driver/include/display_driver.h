#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "battery_driver.h"

#define LCD_WIDTH   320
#define LCD_HEIGHT  240

#define EMO_COLOR_BG            0x0000
#define EMO_COLOR_CARD_BG       0x10A2
#define EMO_COLOR_CARD_BORDER   0x2965
#define EMO_COLOR_CYAN_PRIMARY  0x05F7
#define EMO_COLOR_CYAN_GLOW     0x07FF
#define EMO_COLOR_WHITE         0xFFFF
#define EMO_COLOR_MUTED_TEXT    0x8410
#define EMO_COLOR_HEART_PINK    0xF81F
#define EMO_COLOR_ANGRY_RED     0xF800
#define EMO_COLOR_HUNGRY_ORANGE 0xFD20
#define EMO_COLOR_SAD_BLUE      0x001F
#define EMO_COLOR_ACCENT_GREEN  0x07E0

typedef enum {
    EMO_ANIM_IDLE = 0,
    EMO_ANIM_BLINK,
    EMO_ANIM_LOOK_L,
    EMO_ANIM_LOOK_R,
    EMO_ANIM_HEARTS,
    EMO_ANIM_EATING,
    EMO_ANIM_HUNGRY,
    EMO_ANIM_SAD,
    EMO_ANIM_ANGRY,
    EMO_ANIM_SLEEPY,
    EMO_ANIM_LISTENING,
    EMO_ANIM_SPEAKING
} emo_face_anim_t;

typedef struct {
    int affection;
    int hunger;
    int energy;
    int total_chats;
    int total_pats;
    int total_feeds;
} emo_relationship_t;

esp_err_t display_controller_init(void);
void display_draw_fast_ram_dock(bool is_expanded);
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