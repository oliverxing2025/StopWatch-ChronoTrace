#pragma once

#include <stdint.h>

#include "esp_err.h"

// Initializes the StopWatch CO5300 panel after board_power_init().
esp_err_t display_init(void);

// Double-buffered internal-SRAM DMA band interface used by the renderer.
uint16_t *display_acquire_band(void);
esp_err_t display_flush_band(int band_index, const uint16_t *buffer);

// AMOLED register brightness, 0..255.
esp_err_t display_set_brightness(uint8_t level);
