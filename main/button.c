#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_timer.h"

#define BUTTON_A_GPIO GPIO_NUM_2
#define BUTTON_B_GPIO GPIO_NUM_1
#define DEBOUNCE_SAMPLES 2
#define LONG_PRESS_MS 900
#define DOUBLE_CLICK_MS 360

typedef struct {
    gpio_num_t gpio;
    bool stable;
    int agree_count;
    int64_t pressed_us;
    bool long_sent;
    bool suppressed;
} key_state_t;

static key_state_t s_a = {.gpio = BUTTON_A_GPIO};
static key_state_t s_b = {.gpio = BUTTON_B_GPIO};
static bool s_chord_sent;
static bool s_a_short_pending;
static int64_t s_a_short_us;
static bool s_b_short_pending;
static int64_t s_b_short_us;

static void update_key(key_state_t *key, int64_t now, bool *released_short)
{
    *released_short = false;
    const bool raw = gpio_get_level(key->gpio) == 0;
    if (raw == key->stable) {
        key->agree_count = 0;
        return;
    }
    if (++key->agree_count < DEBOUNCE_SAMPLES) {
        return;
    }

    key->agree_count = 0;
    key->stable = raw;
    if (raw) {
        key->pressed_us = now;
        key->long_sent = false;
        key->suppressed = false;
    } else {
        *released_short = !key->long_sent && !key->suppressed;
    }
}

esp_err_t button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BUTTON_A_GPIO) | (1ULL << BUTTON_B_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

button_event_t button_poll(void)
{
    const int64_t now = esp_timer_get_time();
    bool a_short = false;
    bool b_short = false;
    update_key(&s_a, now, &a_short);
    update_key(&s_b, now, &b_short);

    if (s_a.stable && s_b.stable) {
        s_a_short_pending = false;
        s_b_short_pending = false;
        s_a.suppressed = true;
        s_b.suppressed = true;
        const int64_t chord_start = s_a.pressed_us > s_b.pressed_us ?
                                    s_a.pressed_us : s_b.pressed_us;
        if (!s_chord_sent && (now - chord_start) / 1000 >= LONG_PRESS_MS) {
            s_chord_sent = true;
            return BUTTON_EVENT_AB_LONG;
        }
        return BUTTON_EVENT_NONE;
    }
    if (!s_a.stable && !s_b.stable) {
        s_chord_sent = false;
    }

    if (a_short) {
        if (s_a_short_pending &&
            (now - s_a_short_us) / 1000 <= DOUBLE_CLICK_MS) {
            s_a_short_pending = false;
            return BUTTON_EVENT_A_DOUBLE;
        }
        if (s_a_short_pending) {
            s_a_short_us = now;
            return BUTTON_EVENT_A_SHORT;
        }
        s_a_short_pending = true;
        s_a_short_us = now;
    }
    if (b_short) {
        if (s_b_short_pending &&
            (now - s_b_short_us) / 1000 <= DOUBLE_CLICK_MS) {
            s_b_short_pending = false;
            return BUTTON_EVENT_B_DOUBLE;
        }
        s_b_short_pending = true;
        s_b_short_us = now;
    }

    if (s_a.stable && !s_a.long_sent && !s_a.suppressed &&
        (now - s_a.pressed_us) / 1000 >= LONG_PRESS_MS) {
        s_a_short_pending = false;
        s_a.long_sent = true;
        return BUTTON_EVENT_A_LONG;
    }
    if (s_b.stable && !s_b.long_sent && !s_b.suppressed &&
        (now - s_b.pressed_us) / 1000 >= LONG_PRESS_MS) {
        s_b_short_pending = false;
        s_b.long_sent = true;
        return BUTTON_EVENT_B_LONG;
    }
    if (s_a_short_pending && !s_a.stable &&
        (now - s_a_short_us) / 1000 > DOUBLE_CLICK_MS) {
        s_a_short_pending = false;
        return BUTTON_EVENT_A_SHORT;
    }
    if (s_b_short_pending && !s_b.stable &&
        (now - s_b_short_us) / 1000 > DOUBLE_CLICK_MS) {
        s_b_short_pending = false;
        return BUTTON_EVENT_B_SHORT;
    }
    return BUTTON_EVENT_NONE;
}
