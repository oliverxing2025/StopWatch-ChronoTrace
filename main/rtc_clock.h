#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"

esp_err_t rtc_clock_init(i2c_bus_handle_t bus);
esp_err_t rtc_clock_read(uint8_t *hours, uint8_t *minutes);
esp_err_t rtc_clock_read_hms(uint8_t *hours, uint8_t *minutes, uint8_t *seconds);
// Calibrates the hardware RTC from Unix UTC seconds and a phone-provided
// timezone offset. Alarm/control registers are left untouched.
esp_err_t rtc_clock_set_unix(int64_t unix_seconds, int16_t timezone_minutes);
