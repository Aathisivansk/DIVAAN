#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif