/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 lbuque <1102390310@qq.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bmi270_bmm150.h"

static const char *TAG = "BMI270_BMM150_DEMO";

/* 常见 ESP32 开发板 I2C 引脚，可按你的硬件修改 */
#define I2C_MASTER_SCL_IO    33
#define I2C_MASTER_SDA_IO    32
#define I2C_MASTER_PORT      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ   400000


/* Inside .cpp file, app_main function must be declared with C linkage */
extern "C" void app_main(void) {
    i2c_config_t i2c_bus_conf;
    memset(&i2c_bus_conf, 0, sizeof(i2c_bus_conf));
    i2c_bus_conf.mode = I2C_MODE_MASTER;
    i2c_bus_conf.sda_io_num = I2C_MASTER_SDA_IO;
    i2c_bus_conf.scl_io_num = I2C_MASTER_SCL_IO;
    i2c_bus_conf.sda_pullup_en = true;
    i2c_bus_conf.scl_pullup_en = true;
    i2c_bus_conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    i2c_bus_handle_t i2c_bus = i2c_bus_create(I2C_MASTER_PORT, &i2c_bus_conf);
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "i2c_bus_create failed");
        return;
    }

    // uint8_t scan_result[128];
    // uint8_t num_devices = i2c_bus_scan(i2c_bus, scan_result, sizeof(scan_result));
    // ESP_LOGI(TAG, "I2C scan found %d devices:", num_devices);
    // for (int i = 0; i < num_devices; i++) {
    //     ESP_LOGI(TAG, " - Device at 0x%02X", scan_result[i]);
    // }

    bmi270_bmm150_config_t sensor_conf = {
        .i2c_addr = BMI2_I2C_PRIM_ADDR,    // BMI270: 0x68
        .config_file_ptr = NULL,           // Use default config file
        .mode = BOSCH_ACCEL_AND_MAGN,
    };

    bmi270_bmm150_handle_t sensor = NULL;
    esp_err_t ret = bmi270_bmm150_sensor_create(i2c_bus, &sensor, &sensor_conf);
    if (ret != ESP_OK || sensor == NULL) {
        ESP_LOGE(TAG, "bmi270_bmm150_sensor_create failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "BMI270 init done, start reading acceleration...");

    while (1) {
        float ax = 0, ay = 0, az = 0;
        int available = 0;
        ret = bmi270_bmm150_sensor_acceleration_available(sensor, &available);
        if (ret == ESP_OK && available > 0) {
            ret = bmi270_bmm150_sensor_read_acceleration(sensor, &ax, &ay, &az);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "ACC[g]: x=%.3f, y=%.3f, z=%.3f", ax, ay, az);
            } else {
                ESP_LOGW(TAG, "read acceleration failed: %s", esp_err_to_name(ret));
            }
        }
        ret = bmi270_bmm150_sensor_gyroscope_available(sensor, &available);
        if (ret == ESP_OK && available > 0) {
            float gx = 0, gy = 0, gz = 0;
            ret = bmi270_bmm150_sensor_read_gyroscope(sensor, &gx, &gy, &gz);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "GYR[dps]: x=%.3f, y=%.3f, z=%.3f", gx, gy, gz);
            } else {
                ESP_LOGW(TAG, "read gyroscope failed: %s", esp_err_to_name(ret));
            }
        }
        ret = bmi270_bmm150_sensor_magnetic_field_available(sensor, &available);
        if (ret == ESP_OK && available > 0) {
            float mx = 0, my = 0, mz = 0;
            ret = bmi270_bmm150_sensor_read_magnetic_field(sensor, &mx, &my, &mz);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "MAG[uT]: x=%.3f, y=%.3f, z=%.3f", mx, my, mz);
            } else {
                ESP_LOGW(TAG, "read magnetic field failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
