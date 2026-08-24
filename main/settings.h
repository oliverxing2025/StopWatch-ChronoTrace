#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t theme;       // 0..7
    uint8_t volume;      // continuous 0..100 percent
    uint8_t density;     // 0=light, 1=standard, 2=dense
    bool reactive;       // external-microphone music visualisation
    uint8_t language;    // 0=Simplified Chinese, 1=English
    bool onboarding_complete;
    bool bluetooth_enabled;
    bool wifi_enabled;
    uint8_t brightness;  // continuous 0..100 percent
    bool haptic_enabled;
} chrono_trace_settings_t;

// Never erases NVS on an error: the partition may contain settings belonging
// to the factory firmware. Defaults remain usable if initialization fails.
esp_err_t settings_init(chrono_trace_settings_t *settings);
esp_err_t settings_save(const chrono_trace_settings_t *settings);
esp_err_t settings_load_handwriting(uint8_t *count, uint8_t *data,
                                    size_t capacity, size_t glyph_bytes,
                                    uint8_t *colors, size_t color_capacity);
esp_err_t settings_save_handwriting(uint8_t count, const uint8_t *data,
                                    size_t glyph_bytes, const uint8_t *colors);
