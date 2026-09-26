#include "touch_sensor.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static int s_p1 = 1, s_p2 = 2, s_p3 = 3, s_p4 = 5;
static int64_t s_hold_start = 0;
static int64_t s_last_swipe_t = 0;
static int64_t s_last_tap_p1 = 0;
static int64_t s_last_tap_p4 = 0;
static bool s_is_holding = false;
static bool s_prev_p1 = false, s_prev_p2 = false, s_prev_p3 = false, s_prev_p4 = false;
static int s_swipe_step = 0;

esp_err_t touch_sensor_init(int p1, int p2, int p3, int p4) {
    s_p1 = p1; s_p2 = p2; s_p3 = p3; s_p4 = p4;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << p1) | (1ULL << p2) | (1ULL << p3) | (1ULL << p4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

bool touch_sensor_is_head_held(void) {
    return (gpio_get_level(s_p2) == 1 || gpio_get_level(s_p3) == 1);
}

touch_gesture_t touch_sensor_update_and_poll(void) {
    int64_t now = esp_timer_get_time() / 1000;
    bool t1 = gpio_get_level(s_p1) == 1;
    bool t2 = gpio_get_level(s_p2) == 1;
    bool t3 = gpio_get_level(s_p3) == 1;
    bool t4 = gpio_get_level(s_p4) == 1;
    bool tm = (t2 || t3);
    bool prev_tm = (s_prev_p2 || s_prev_p3);

    // 1. Double tap left cheek tickle (Pad 1)
    if (t1 && !s_prev_p1) {
        if (now - s_last_tap_p1 < 380) {
            s_prev_p1 = t1;
            return TOUCH_EVENT_TICKLE_LEFT;
        }
        s_last_tap_p1 = now;
    }

    // 2. Double tap right cheek tickle (Pad 4)
    if (t4 && !s_prev_p4) {
        if (now - s_last_tap_p4 < 380) {
            s_prev_p4 = t4;
            return TOUCH_EVENT_TICKLE_RIGHT;
        }
        s_last_tap_p4 = now;
    }

    // 3. 4-Pad Head Petting Sweep (P1 -> P2/P3 -> P4 or P4 -> P3/P2 -> P1)
    if (t1 && s_swipe_step == 0) { s_swipe_step = 1; s_last_swipe_t = now; }
    else if (t4 && s_swipe_step == 0) { s_swipe_step = 3; s_last_swipe_t = now; }

    if (now - s_last_swipe_t < 650) {
        if (tm && (s_swipe_step == 1 || s_swipe_step == 3)) s_swipe_step = 2;
        else if ((t4 && s_swipe_step == 2) || (t1 && s_swipe_step == 2)) {
            s_swipe_step = 0;
            s_is_holding = false;
            return TOUCH_EVENT_PATTED;
        }
    } else {
        s_swipe_step = 0;
    }

    // 4. Middle Dual-Sensor Hold-to-Talk (Pad 2 or Pad 3)
    if (tm && !prev_tm) {
        s_hold_start = now;
    } else if (tm && prev_tm) {
        if (!s_is_holding && (now - s_hold_start >= 350) && s_swipe_step == 0) {
            s_is_holding = true;
            s_prev_p1 = t1; s_prev_p2 = t2; s_prev_p3 = t3; s_prev_p4 = t4;
            return TOUCH_EVENT_HOLD_START;
        }
    } else if (!tm && prev_tm) {
        if (s_is_holding) {
            s_is_holding = false;
            s_prev_p1 = t1; s_prev_p2 = t2; s_prev_p3 = t3; s_prev_p4 = t4;
            return TOUCH_EVENT_HOLD_RELEASE;
        }
        s_hold_start = 0;
    }

    s_prev_p1 = t1; s_prev_p2 = t2; s_prev_p3 = t3; s_prev_p4 = t4;
    return TOUCH_EVENT_NONE;
}