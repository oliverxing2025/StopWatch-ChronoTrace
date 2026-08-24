#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

// Enables the StopWatch lower-board rail and releases the AMOLED reset through
// M5IOE1. The existing state of every unrelated expander output is preserved.
esp_err_t board_power_init(i2c_bus_handle_t i2c_bus);

// Controls the ES8311 speaker path through the M5IOE1 and GPIO14 power amps.
esp_err_t board_speaker_enable(bool enable);
esp_err_t board_touch_reset(void);

// A restrained, non-blocking StopWatch vibration pulse for touch feedback.
// Uses the official motor path: M5IOE1 IO9 / PWM channel 1 at 2 kHz.
void board_haptic_click(void);

#ifdef __cplusplus
}
#endif
