#include "display_driver.h"
#include "config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

#define EYE_BOX_W   88
#define EYE_BOX_H   100

static spi_device_handle_t s_spi_lcd;
static spi_device_handle_t s_spi_touch;

static uint16_t s_eye_dma_buf[EYE_BOX_W * EYE_BOX_H];
static int s_prev_lx = 54, s_prev_ly = 40;
static int s_prev_rx = 184, s_prev_ry = 40;

static char s_last_header_str[48] = {0};
static int s_last_bat_bars = -1;
static bool s_last_bat_chg = false;
static bool s_last_wifi_state = false;

#define DOCK_ICON_SIZE 40

static emo_relationship_t s_metrics = {
    .affection = 85,
    .hunger = 75,
    .energy = 90,
    .total_chats = 0,
    .total_pats = 0,
    .total_feeds = 0
};

static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(PIN_NUM_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi_lcd, &t);
}

static void lcd_data(const uint8_t *data, int len) {
    if (len <= 0) return;
    gpio_set_level(PIN_NUM_LCD_DC, 1);
    spi_transaction_t t = { .length = (size_t)len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi_lcd, &t);
}

static void lcd_set_window(int x0, int y0, int x1, int y1) {
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    lcd_cmd(0x2A); lcd_data(caset, 4);
    lcd_cmd(0x2B); lcd_data(raset, 4);
    lcd_cmd(0x2C);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w <= 0 || h <= 0) return;
    int x1 = x + w - 1; if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    int y1 = y + h - 1; if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    lcd_set_window(x, y, x1, y1);
    int total = (x1 - x + 1) * (y1 - y + 1);
    uint16_t line_buf[128];
    uint16_t swapped = (uint16_t)((color << 8) | (color >> 8));
    for (int i = 0; i < 128; i++) line_buf[i] = swapped;
    gpio_set_level(PIN_NUM_LCD_DC, 1);
    while (total > 0) {
        int chunk = total > 128 ? 128 : total;
        spi_transaction_t t = { .length = (size_t)chunk * 16, .tx_buffer = line_buf };
        spi_device_polling_transmit(s_spi_lcd, &t);
        total -= chunk;
    }
}

void display_draw_rect_outline(int x, int y, int w, int h, uint16_t color) {
    display_fill_rect(x, y, w, 1, color);
    display_fill_rect(x, y + h - 1, w, 1, color);
    display_fill_rect(x, y, 1, h, color);
    display_fill_rect(x + w - 1, y, 1, h, color);
}

void display_clear_all(void) {
    display_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, EMO_COLOR_BG);
    s_last_header_str[0] = '\0';
}

static inline void eye_buf_set_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= EYE_BOX_W || y < 0 || y >= EYE_BOX_H) return;
    s_eye_dma_buf[y * EYE_BOX_W + x] = (uint16_t)((color << 8) | (color >> 8));
}

static void eye_buf_fill_round_rect(int rx, int ry, int rw, int rh, int rad, uint16_t color) {
    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            bool draw = true;
            if (x < rad && y < rad) {
                if ((rad - x) * (rad - x) + (rad - y) * (rad - y) > rad * rad) draw = false;
            } else if (x >= rw - rad && y < rad) {
                int dx = x - (rw - rad - 1);
                if (dx * dx + (rad - y) * (rad - y) > rad * rad) draw = false;
            } else if (x < rad && y >= rh - rad) {
                int dy = y - (rh - rad - 1);
                if ((rad - x) * (rad - x) + dy * dy > rad * rad) draw = false;
            } else if (x >= rw - rad && y >= rh - rad) {
                int dx = x - (rw - rad - 1);
                int dy = y - (rh - rad - 1);
                if (dx * dx + dy * dy > rad * rad) draw = false;
            }
            if (draw) {
                eye_buf_set_pixel(rx + x, ry + y, color);
            }
        }
    }
}

static void render_and_send_eye(int cur_x, int cur_y, int old_x, int old_y, int eye_w, int eye_h, int rad, uint16_t color, bool heart, bool wink, int slant_dir) {
    if (cur_x > old_x) display_fill_rect(old_x, old_y, cur_x - old_x, EYE_BOX_H, EMO_COLOR_BG);
    if (cur_x < old_x) display_fill_rect(cur_x + EYE_BOX_W, old_y, old_x - cur_x, EYE_BOX_H, EMO_COLOR_BG);
    if (cur_y > old_y) display_fill_rect(old_x, old_y, EYE_BOX_W, cur_y - old_y, EMO_COLOR_BG);
    if (cur_y < old_y) display_fill_rect(old_x, cur_y + EYE_BOX_H, EYE_BOX_W, old_y - cur_y, EMO_COLOR_BG);

    memset(s_eye_dma_buf, 0, sizeof(s_eye_dma_buf));
    int off_x = (EYE_BOX_W - eye_w) / 2;
    int off_y = (EYE_BOX_H - eye_h) / 2;

    if (wink) {
        eye_buf_fill_round_rect(off_x, off_y + 35, eye_w, 14, 6, color);
    } else if (heart) {
        eye_buf_fill_round_rect(off_x + 10, off_y + 12, 28, 28, 14, EMO_COLOR_HEART_PINK);
        eye_buf_fill_round_rect(off_x + 36, off_y + 12, 28, 28, 14, EMO_COLOR_HEART_PINK);
        for (int y = 0; y < 35; y++) {
            for (int x = y; x < 54 - y; x++) {
                eye_buf_set_pixel(off_x + 10 + x, off_y + 32 + y, EMO_COLOR_HEART_PINK);
            }
        }
    } else {
        eye_buf_fill_round_rect(off_x, off_y, eye_w, eye_h, rad, color);
        if (slant_dir == 1) {
            for (int y = 0; y < 24; y++) {
                for (int x = 0; x < 36 - y; x++) eye_buf_set_pixel(off_x + x, off_y + y, EMO_COLOR_BG);
            }
        } else if (slant_dir == -1) {
            for (int y = 0; y < 24; y++) {
                for (int x = eye_w - (36 - y); x < eye_w; x++) eye_buf_set_pixel(off_x + x, off_y + y, EMO_COLOR_BG);
            }
        } else if (slant_dir == 2) {
            for (int y = 0; y < 18; y++) {
                for (int x = 0; x < eye_w; x++) {
                    if (y < (x / 3)) eye_buf_set_pixel(off_x + x, off_y + y, EMO_COLOR_BG);
                }
            }
        }
        if (eye_w >= 36 && eye_h >= 36 && slant_dir == 0) {
            eye_buf_fill_round_rect(off_x + 8, off_y + 8, eye_w - 16, eye_h - 16, rad / 2, EMO_COLOR_CYAN_GLOW);
            eye_buf_fill_round_rect(off_x + 12, off_y + 12, 10, 10, 4, EMO_COLOR_WHITE);
        }
    }

    lcd_set_window(cur_x, cur_y, cur_x + EYE_BOX_W - 1, cur_y + EYE_BOX_H - 1);
    gpio_set_level(PIN_NUM_LCD_DC, 1);
    spi_transaction_t t = {
        .length = EYE_BOX_W * EYE_BOX_H * 16,
        .tx_buffer = s_eye_dma_buf
    };
    spi_device_polling_transmit(s_spi_lcd, &t);
}

// ---------------- Font Engine ----------------
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
    0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
    0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, 0x00,0x1C,0x22,0x41,0x00,
    0x00,0x41,0x22,0x1C,0x00, 0x08,0x2A,0x1C,0x2A,0x08, 0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00,
    0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
    0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, 0x18,0x14,0x12,0x7F,0x10,
    0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00,
    0x00,0x56,0x36,0x00,0x00, 0x08,0x14,0x22,0x41,0x00, 0x14,0x14,0x14,0x14,0x14,
    0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x51,0x09,0x06, 0x32,0x49,0x79,0x41,0x3E,
    0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x01,0x01,
    0x3E,0x41,0x49,0x49,0x7A, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
    0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
    0x7F,0x02,0x0C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
    0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F,
    0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F, 0x63,0x14,0x08,0x14,0x63,
    0x07,0x08,0x70,0x08,0x07, 0x61,0x51,0x49,0x45,0x43, 0x00,0x7F,0x41,0x41,0x00,
    0x00,0x41,0x41,0x7F,0x00, 0x00,0x00,0x7F,0x00,0x00
};

void display_draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (!str) return;
    while (*str) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') c -= 32;
        int idx = 0;
        if (c >= 32 && c <= 90) idx = (c - 32) * 5;
        else if (c == '[') idx = 59 * 5;
        else if (c == ']') idx = 60 * 5;
        else if (c == '|') idx = 61 * 5;
        else if (c == '<') idx = 28 * 5;
        else if (c == '>') idx = 30 * 5;
        else idx = 0;

        for (int col = 0; col < 5; col++) {
            uint8_t line = (idx + col < (int)sizeof(font5x7)) ? font5x7[idx + col] : 0;
            for (int row = 0; row < 7; row++) {
                if (line & (1 << row)) {
                    display_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                } else if (bg != fg) {
                    display_fill_rect(x + col * scale, y + row * scale, scale, scale, bg);
                }
            }
        }
        x += 6 * scale;
    }
}

static void emo_draw_battery_icon_internal(int16_t x, int16_t y, battery_status_t bat, int tick) {
    display_fill_rect(x, y, 22, 12, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(x, y, 22, 12, EMO_COLOR_CARD_BORDER);
    display_fill_rect(x + 22, y + 3, 2, 6, EMO_COLOR_CARD_BORDER);

    if (bat.is_charging) {
        uint16_t chg_col = ((tick / 4) % 2 == 0) ? 0xFFE0 : EMO_COLOR_CYAN_GLOW;
        display_draw_text(x + 6, y + 2, "*", chg_col, EMO_COLOR_CARD_BG, 1);
        return;
    }

    if (bat.is_critical) {
        if ((tick / 4) % 2 == 0) display_fill_rect(x + 2, y + 2, 18, 8, 0xF800);
        else display_fill_rect(x + 2, y + 2, 18, 8, EMO_COLOR_CARD_BG);
        return;
    }

    uint16_t bar_color = (bat.bars >= 3) ? 0x07E0 : (bat.bars == 2 ? 0xFFE0 : 0xFD20);
    display_fill_rect(x + 2, y + 2, 18, 8, EMO_COLOR_CARD_BG);
    for (int i = 0; i < bat.bars; i++) {
        display_fill_rect(x + 3 + (i * 4), y + 3, 3, 6, bar_color);
    }
}

void emo_draw_header_bar(const char *time_str, const char *date_str, const char *weather_str, bool wifi_ok, battery_status_t bat, int tick) {
    char cur_header[48];
    snprintf(cur_header, sizeof(cur_header), "%s|%s|%s", time_str ? time_str : "", date_str ? date_str : "", weather_str ? weather_str : "");

    if (strcmp(cur_header, s_last_header_str) != 0 || wifi_ok != s_last_wifi_state) {
        display_fill_rect(0, 0, LCD_WIDTH, 22, 0x0841);
        if (time_str) display_draw_text(8, 5, time_str, EMO_COLOR_CYAN_GLOW, 0x0841, 2);
        if (date_str) display_draw_text(110, 7, date_str, 0xC618, 0x0841, 1);
        if (weather_str) display_draw_text(195, 7, weather_str, 0xFFE0, 0x0841, 1);

        uint16_t wc = wifi_ok ? 0x07E0 : 0xF800;
        display_fill_rect(265, 13, 3, 4, wc);
        display_fill_rect(269, 10, 3, 7, wc);
        display_fill_rect(273, 7, 3, 10, wc);

        strncpy(s_last_header_str, cur_header, sizeof(s_last_header_str) - 1);
        s_last_wifi_state = wifi_ok;
        emo_draw_battery_icon_internal(286, 5, bat, tick);
    } else if (bat.bars != s_last_bat_bars || bat.is_charging != s_last_bat_chg || bat.is_charging || bat.is_critical) {
        emo_draw_battery_icon_internal(286, 5, bat, tick);
    }

    s_last_bat_bars = bat.bars;
    s_last_bat_chg = bat.is_charging;
}

// ---------------- High-Definition Procedural Dock Icons (Zero Disk/Files) ----------------
static void draw_dock_icon_graphic(int x, int y, int icon_type) {
    display_fill_rect(x, y, DOCK_ICON_SIZE, DOCK_ICON_SIZE, EMO_COLOR_CARD_BG);
    display_draw_rect_outline(x, y, DOCK_ICON_SIZE, DOCK_ICON_SIZE, EMO_COLOR_CARD_BORDER);

    switch (icon_type) {
        case 0: // Alarm Clock (Bell Icon)
            display_fill_rect(x + 14, y + 10, 12, 14, 0xFFE0);
            display_fill_rect(x + 10, y + 24, 20, 4, 0xFFE0);
            display_fill_rect(x + 18, y + 28, 4, 3, 0xF800);
            display_draw_rect_outline(x + 12, y + 8, 4, 4, 0xFFE0);
            display_draw_rect_outline(x + 24, y + 8, 4, 4, 0xFFE0);
            break;

        case 1: // Focus Timer (Target Reticle)
            display_draw_rect_outline(x + 10, y + 10, 20, 20, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 18, y + 18, 4, 4, 0x07E0);
            display_fill_rect(x + 19, y + 6, 2, 5, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 19, y + 29, 2, 5, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 6, y + 19, 5, 2, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 29, y + 19, 5, 2, EMO_COLOR_CYAN_GLOW);
            break;

        case 2: // Tic-Tac-Toe (Grid + X + O)
            display_fill_rect(x + 16, y + 8, 2, 24, 0x8410);
            display_fill_rect(x + 24, y + 8, 2, 24, 0x8410);
            display_fill_rect(x + 8, y + 16, 24, 2, 0x8410);
            display_fill_rect(x + 8, y + 24, 24, 2, 0x8410);
            display_draw_text(x + 9, y + 9, "X", 0xF800, EMO_COLOR_CARD_BG, 1);
            display_draw_text(x + 27, y + 17, "O", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);
            break;

        case 3: // Quick Tap (Lightning Bolt / Touch Hand)
            display_fill_rect(x + 18, y + 8, 6, 10, 0xFFE0);
            display_fill_rect(x + 12, y + 18, 16, 4, 0xFFE0);
            display_fill_rect(x + 16, y + 22, 6, 10, 0xFFE0);
            display_fill_rect(x + 18, y + 32, 2, 2, 0xFFE0);
            break;

        case 4: // Media Player (Musical Note)
            display_fill_rect(x + 14, y + 22, 6, 6, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 24, y + 18, 6, 6, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 18, y + 10, 2, 14, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 28, y + 8, 2, 12, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(x + 18, y + 8, 12, 4, EMO_COLOR_CYAN_GLOW);
            break;
    }
}

void display_draw_fast_ram_dock(bool is_expanded) {
    if (!is_expanded) {
        display_fill_rect(10, 180, 300, 58, EMO_COLOR_BG);
        display_fill_rect(130, 214, 60, 22, EMO_COLOR_CARD_BG);
        display_draw_rect_outline(130, 214, 60, 22, EMO_COLOR_CYAN_GLOW);
        display_draw_text(154, 218, "^", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 2);
    } else {
        display_fill_rect(10, 182, 300, 54, EMO_COLOR_CARD_BG);
        display_draw_rect_outline(10, 182, 300, 54, EMO_COLOR_CYAN_GLOW);

        draw_dock_icon_graphic(20, 189, 0);  // Alarm
        draw_dock_icon_graphic(78, 189, 1);  // Focus
        draw_dock_icon_graphic(136, 189, 2); // TTT
        draw_dock_icon_graphic(194, 189, 3); // Tap
        draw_dock_icon_graphic(252, 189, 4); // Music
    }
}

static void draw_mouth(int state, int tick) {
    display_fill_rect(130, 136, 60, 24, EMO_COLOR_BG);
    switch (state) {
        case 1: {
            int mw = 20 + (int)(fabsf(sinf(tick * 0.8f)) * 22.0f);
            int mh = 6 + (int)(fabsf(sinf(tick * 0.8f)) * 12.0f);
            display_fill_rect(160 - (mw / 2), 145 - (mh / 2), mw, mh, EMO_COLOR_CYAN_GLOW);
            break;
        }
        case 2:
            display_fill_rect(142, 144, 36, 5, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(140, 140, 5, 7, EMO_COLOR_CYAN_GLOW);
            display_fill_rect(175, 140, 5, 7, EMO_COLOR_CYAN_GLOW);
            break;
        case 4: {
            int ch_h = 4 + ((tick % 4) * 3);
            display_fill_rect(146, 144 - (ch_h / 2), 28, ch_h, 0x07E0);
            break;
        }
        case 7:
            display_fill_rect(146, 142, 28, 6, EMO_COLOR_HUNGRY_ORANGE);
            display_fill_rect(154, 148, 12, 8, EMO_COLOR_HEART_PINK);
            break;
        default:
            display_fill_rect(156, 146, 8, 4, EMO_COLOR_CYAN_PRIMARY);
            break;
    }
}

void emo_draw_face(emo_face_anim_t anim, int tick) {
    int cur_lx = 54, cur_rx = 184, cur_y = 38;
    int ew = 74, eh = 86, rad = 24;
    bool is_heart = false, is_wink_l = false, is_wink_r = false;
    int slant_l = 0, slant_r = 0;
    int mouth_mode = 0;
    uint16_t color = EMO_COLOR_CYAN_PRIMARY;

    switch (anim) {
        case EMO_ANIM_IDLE:
            cur_y += (int)(sinf(tick * 0.12f) * 2.5f);
            break;
        case EMO_ANIM_BLINK:
            if (tick % 8 == 1 || tick % 8 == 3) { eh = 18; rad = 6; }
            else if (tick % 8 == 2) { eh = 4; rad = 2; }
            break;
        case EMO_ANIM_LOOK_L:
            cur_lx = 28; cur_rx = 158;
            break;
        case EMO_ANIM_LOOK_R:
            cur_lx = 80; cur_rx = 210;
            break;
        case EMO_ANIM_HEARTS:
            is_heart = true;
            mouth_mode = 2;
            break;
        case EMO_ANIM_EATING:
            cur_y += (int)(sinf(tick * 0.5f) * 3.0f);
            mouth_mode = 4;
            break;
        case EMO_ANIM_HUNGRY:
            eh = 60;
            color = EMO_COLOR_HUNGRY_ORANGE;
            mouth_mode = 7;
            break;
        case EMO_ANIM_SAD:
            eh = 64; rad = 14;
            slant_l = 2; slant_r = 2;
            color = EMO_COLOR_SAD_BLUE;
            break;
        case EMO_ANIM_SLEEPY:
            eh = 34; rad = 8;
            color = 0x0375;
            break;
        case EMO_ANIM_LISTENING:
            eh = 46 + (int)(fabsf(sinf(tick * 0.4f)) * 36.0f);
            color = 0x07E0;
            break;
        case EMO_ANIM_SPEAKING:
            cur_y += (int)(sinf(tick * 0.6f) * 4.0f);
            mouth_mode = 1;
            break;
        default:
            break;
    }

    render_and_send_eye(cur_lx, cur_y, s_prev_lx, s_prev_ly, ew, eh, rad, color, is_heart, is_wink_l, slant_l);
    render_and_send_eye(cur_rx, cur_y, s_prev_rx, s_prev_ly, ew, eh, rad, color, is_heart, is_wink_r, slant_r);
    s_prev_lx = cur_lx; s_prev_ly = cur_y;
    s_prev_rx = cur_rx; s_prev_ry = cur_y;
    draw_mouth(mouth_mode, tick);
}

void emo_draw_sd_notification_popup(const char *app_name, const char *message) {
    display_fill_rect(16, 24, 288, 76, EMO_COLOR_CARD_BORDER);
    display_fill_rect(18, 26, 284, 72, EMO_COLOR_CARD_BG);
    display_fill_rect(26, 38, 48, 48, EMO_COLOR_CYAN_GLOW);
    display_draw_text(34, 54, "NOTIF", EMO_COLOR_BG, EMO_COLOR_CYAN_GLOW, 1);
    display_draw_text(84, 36, app_name ? app_name : "NOTIFICATION", EMO_COLOR_CYAN_GLOW, EMO_COLOR_CARD_BG, 1);

    if (message) {
        char line1[24] = {0};
        char line2[24] = {0};
        int len = strlen(message);
        if (len <= 18) {
            strncpy(line1, message, sizeof(line1) - 1);
            display_draw_text(84, 54, line1, EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
        } else {
            strncpy(line1, message, 18);
            strncpy(line2, message + 18, sizeof(line2) - 1);
            display_draw_text(84, 50, line1, EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
            display_draw_text(84, 66, line2, EMO_COLOR_WHITE, EMO_COLOR_CARD_BG, 1);
        }
    }
}

bool display_get_touch(int16_t *out_x, int16_t *out_y) {
    uint8_t tx_x[3] = {0x90, 0x00, 0x00};
    uint8_t rx_x[3] = {0};
    uint8_t tx_y[3] = {0xD0, 0x00, 0x00};
    uint8_t rx_y[3] = {0};

    spi_transaction_t t_x = { .length = 24, .tx_buffer = tx_x, .rx_buffer = rx_x };
    spi_transaction_t t_y = { .length = 24, .tx_buffer = tx_y, .rx_buffer = rx_y };
    spi_device_polling_transmit(s_spi_touch, &t_x);
    spi_device_polling_transmit(s_spi_touch, &t_y);

    int16_t raw_x = (int16_t)((rx_x[1] << 5) | (rx_x[2] >> 3));
    int16_t raw_y = (int16_t)((rx_y[1] << 5) | (rx_y[2] >> 3));

    if (raw_x < 200 || raw_x > 3900 || raw_y < 200 || raw_y > 3900) return false;

    int16_t px = (int16_t)(((raw_x - 200) * LCD_WIDTH) / 3700);
    int16_t py = (int16_t)(((raw_y - 200) * LCD_HEIGHT) / 3700);

    if (px < 0) px = 0;
    if (px >= LCD_WIDTH) px = LCD_WIDTH - 1;
    if (py < 0) py = 0;
    if (py >= LCD_HEIGHT) py = LCD_HEIGHT - 1;

    if (out_x) *out_x = px;
    if (out_y) *out_y = py;
    return true;
}

esp_err_t display_controller_init(void) {
    gpio_set_direction(PIN_NUM_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_LCD_BCKL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_LCD_BCKL, 1);

    gpio_set_level(PIN_NUM_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SPI_CLK,
        .mosi_io_num = PIN_NUM_SPI_MOSI,
        .miso_io_num = PIN_NUM_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EYE_BOX_W * EYE_BOX_H * sizeof(uint16_t)
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg_lcd = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_LCD_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_lcd, &s_spi_lcd));

    spi_device_interface_config_t devcfg_touch = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_TOUCH_CS,
        .queue_size = 3,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_touch, &s_spi_touch));

    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    
    uint8_t madctl = 0x60;
    lcd_cmd(0x36); lcd_data(&madctl, 1);
    uint8_t colmod = 0x55;
    lcd_cmd(0x3A); lcd_data(&colmod, 1);
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));

    display_clear_all();
    return ESP_OK;
}

void emo_metrics_init(void) {}
void emo_metrics_save(void) {}
emo_relationship_t* emo_metrics_get(void) { return &s_metrics; }