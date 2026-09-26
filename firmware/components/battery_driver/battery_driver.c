#include "battery_driver.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "BATTERY_ADC";

static adc_oneshot_unit_handle_t s_adc2_handle = NULL;
static adc_cali_handle_t s_adc2_cali_handle = NULL;
static bool s_calibrated = false;

// Voltage Divider: R1 = 100k, R2 = 47k -> Ratio = (100 + 47) / 47 = 3.12766
#define DIVIDER_RATIO 3.12766f

static float s_smoothed_voltage = 7.50f; // Default baseline (prevents null stall)
static int s_current_bars = 2;
static int64_t s_last_adc_read_us = 0;

esp_err_t battery_driver_init(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_adc2_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC2 unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc2_handle, ADC_CHANNEL_3, &config));

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_2,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc2_cali_handle) == ESP_OK) {
        s_calibrated = true;
    }

    ESP_LOGI(TAG, "Battery Monitor Initialized on GPIO 14 (ADC2_CH3)");
    return ESP_OK;
}

battery_status_t battery_get_status(void) {
    battery_status_t status = {
        .voltage = s_smoothed_voltage,
        .bars = s_current_bars,
        .is_charging = (s_smoothed_voltage >= 8.20f),
        .is_critical = (s_smoothed_voltage < 6.60f)
    };

    if (!s_adc2_handle) return status;

    // Rate limit ADC sampling to once every 1.5 seconds (prevents Wi-Fi RF collisions)
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_adc_read_us < 1500000 && s_last_adc_read_us != 0) {
        return status;
    }
    s_last_adc_read_us = now_us;

    int raw = 0;
    // Single non-blocking read attempt
    esp_err_t res = adc_oneshot_read(s_adc2_handle, ADC_CHANNEL_3, &raw);
    if (res != ESP_OK || raw <= 0) {
        return status; // Return last known filtered state safely
    }

    int voltage_mv = 0;
    if (s_calibrated) {
        adc_cali_raw_to_voltage(s_adc2_cali_handle, raw, &voltage_mv);
    } else {
        voltage_mv = (raw * 3100) / 4095;
    }

    float measured_pack_v = ((float)voltage_mv / 1000.0f) * DIVIDER_RATIO;
    if (measured_pack_v < 4.5f || measured_pack_v > 9.5f) {
        return status;
    }

    s_smoothed_voltage = (s_smoothed_voltage * 0.85f) + (measured_pack_v * 0.15f);

    float v = s_smoothed_voltage;
    if (v >= 8.10f) s_current_bars = 4;
    else if (v >= 7.60f) s_current_bars = 3;
    else if (v >= 7.20f) s_current_bars = 2;
    else if (v >= 6.60f) s_current_bars = 1;
    else s_current_bars = 0;

    status.voltage = v;
    status.bars = s_current_bars;
    status.is_charging = (v >= 8.20f);
    status.is_critical = (v < 6.60f);

    return status;
}