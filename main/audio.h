#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EVENT_BOOT = 0,
    AUDIO_EVENT_RESET,
    AUDIO_EVENT_THEME,
    AUDIO_EVENT_DENSITY,
    AUDIO_EVENT_TOUCH,
    AUDIO_EVENT_UI_CLICK,
    AUDIO_EVENT_REACTIVE,
    AUDIO_EVENT_VOLUME,
    AUDIO_EVENT_COUNTDOWN_TICK,
    AUDIO_EVENT_COUNTDOWN,
} audio_event_t;

esp_err_t audio_init(i2c_bus_handle_t bus, uint8_t theme, uint8_t volume,
                     bool reactive);
void audio_set_theme(uint8_t theme);
void audio_set_volume(uint8_t volume);
void audio_set_reactive(bool reactive);
void audio_set_motion(float mean_speed, float max_speed, int wall_hits,
                      int clamped_pairs);
void audio_get_levels(float *bass, float *mid, float *treble);
void audio_trigger(audio_event_t event);
// urgency is 0 for the normal soft tick/tock and 1 for the final five seconds.
void audio_trigger_countdown_tick(uint8_t urgency);

#ifdef __cplusplus
}
#endif
