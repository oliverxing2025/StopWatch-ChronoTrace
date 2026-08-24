#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_TAP,
    TOUCH_EVENT_DOUBLE_TAP,
    TOUCH_EVENT_LONG_PRESS,
    TOUCH_EVENT_SWIPE_LEFT,
    TOUCH_EVENT_SWIPE_RIGHT,
    TOUCH_EVENT_SWIPE_UP,
    TOUCH_EVENT_SWIPE_DOWN,
} touch_event_t;

esp_err_t touch_init(i2c_bus_handle_t bus);

// Poll every 20-30 ms. Coordinates are returned for taps and are in native
// 466x466 display space.
touch_event_t touch_poll(uint16_t *x, uint16_t *y);
esp_err_t touch_read_raw(bool *pressed, uint16_t *x, uint16_t *y);
void touch_cancel_gestures(void);

#ifdef __cplusplus
}
#endif
