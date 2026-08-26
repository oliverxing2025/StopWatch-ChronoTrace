#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

// StopWatch M5PM1 on-demand battery voltage reader.
esp_err_t battery_init(i2c_bus_handle_t bus);
esp_err_t battery_read(uint8_t *percent, uint16_t *millivolts);
// Uses the StopWatch's PM1 VIN reading and active-low CHG_STAT input so an
// attached data cable is not mistaken for active battery charging.
esp_err_t battery_is_charging(bool *charging);

#ifdef __cplusplus
}
#endif
