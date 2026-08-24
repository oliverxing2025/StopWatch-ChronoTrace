#pragma once

#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_A_SHORT,
    BUTTON_EVENT_A_DOUBLE,
    BUTTON_EVENT_A_LONG,
    BUTTON_EVENT_B_SHORT,
    BUTTON_EVENT_B_DOUBLE,
    BUTTON_EVENT_B_LONG,
    BUTTON_EVENT_AB_LONG,
} button_event_t;

// Initializes the two user buttons: A=GPIO2, B=GPIO1, both active low.
// The PMIC power key is deliberately left under power-management control.
esp_err_t button_init(void);

// Poll every 20-30 ms. Short presses are delayed by the 360 ms double-click
// window; long presses fire once at 900 ms. Holding A+B suppresses the
// individual A/B events.
button_event_t button_poll(void);
