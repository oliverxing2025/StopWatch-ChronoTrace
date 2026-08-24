#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "sim.h"

esp_err_t imu_init(i2c_bus_handle_t i2c_bus);
bool imu_read(float dt, sim_forces_t *out);
void imu_raw_accel(float out[3]);
