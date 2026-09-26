#pragma once

// ----------------- Wi-Fi Configuration -----------------
#define EMO_WIFI_SSID           "Aathisivan's Phone"
#define EMO_WIFI_PASSWORD       "AATHI@1104"
#define EMO_WIFI_MAX_RETRY      5

// ----------------- Backend / Tunnel API ----------------
#define EMO_BACKEND_BASE_URL    "https://divaan-backend.onrender.com"
#define VERCEL_BYPASS_SECRET    "divaan_secure_bypass_key_2026"

// ----------------- Hardware GPIO Mapping ----------------
// 4x Capacitive Head/Cheek Touch Sensors
#define PIN_NUM_TOUCH_1         1   // Left Cheek Touch
#define PIN_NUM_TOUCH_2         2   // Mid-Left Head Touch
#define PIN_NUM_TOUCH_3         3   // Mid-Right Head Touch
#define PIN_NUM_TOUCH_4         5   // Right Cheek Touch

// Audio Input / Output
#define PIN_NUM_MIC_ADC         4   // MAX4466 (ADC1_CH3)
#define PIN_NUM_SPEAKER_PDM     15  // PAM8403 Audio In (Hardware SDM)

// Shared SPI2_HOST Bus
#define PIN_NUM_SPI_CLK         12  // SCLK for LCD, Touch & SD
#define PIN_NUM_SPI_MOSI        11  // MOSI / T_DIN
#define PIN_NUM_SPI_MISO        13  // MISO / T_DO

// ST7789 Display Control Lines
#define PIN_NUM_LCD_CS          10  // LCD Chip Select
#define PIN_NUM_LCD_DC          8   // Data / Command Select
#define PIN_NUM_LCD_RST         9   // LCD Reset
#define PIN_NUM_LCD_BCKL        16  // LCD Backlight Control

// Dedicated Chip Selects for Touch & SD
#define PIN_NUM_TOUCH_CS        7   // XPT2046 Screen Touch CS
#define PIN_NUM_SD_CS           6   // MicroSD Card CS
#define SD_MOUNT_POINT          "/sd"

// ----------------- Timing & Audio Constants ------------
#define AUDIO_SAMPLE_RATE_HZ    16000
#define AUDIO_MAX_RECORD_SEC    5    // 5.0s recorded directly to SD card
#define NTP_SERVER              "pool.ntp.org"
#define TIMEZONE_OFFSET_SEC     (5 * 3600 + 1800) // IST (UTC+5:30)