#include "imu.h"

#include <math.h>
#include <string.h>

#include "bmi270_bmm150.h"
#include "config.h"
#include "esp_log.h"

#define BMI270_ADDRESS 0x68
#define G_TO_MPS2 9.80665f
#define DEG_TO_RAD 0.01745329252f
#define TWO_PI 6.28318530718f

static const char *TAG = "imu";
static bmi270_bmm150_handle_t s_sensor;
static bool s_ready;

static float s_lp[3];
static bool s_lp_primed;
static float s_prev_omega[3];
static float s_raw_accel[3];
static float s_accel[3];
static float s_gyro[3];

esp_err_t imu_init(i2c_bus_handle_t i2c_bus)
{
    const bmi270_bmm150_config_t config = {
        .i2c_addr = BMI270_ADDRESS,
        .config_file_ptr = NULL,
        // This mode still initializes both BMI270 accelerometer and gyro; it
        // only skips the absent/unused BMM150 magnetometer path.
        .mode = BOSCH_ACCELEROMETER_ONLY,
    };

    const esp_err_t ret =
        bmi270_bmm150_sensor_create(i2c_bus, &s_sensor, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BMI270 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ready = true;
    ESP_LOGI(TAG, "BMI270 ready at 0x%02x", BMI270_ADDRESS);
    return ESP_OK;
}

void imu_raw_accel(float out[3])
{
    memcpy(out, s_raw_accel, sizeof(s_raw_accel));
}

bool imu_read(float dt, sim_forces_t *out)
{
    if (!s_ready || out == NULL) {
        return false;
    }

    int accel_available = 0;
    int gyro_available = 0;
    if (bmi270_bmm150_sensor_acceleration_available(
            s_sensor, &accel_available) != ESP_OK ||
        bmi270_bmm150_sensor_gyroscope_available(
            s_sensor, &gyro_available) != ESP_OK ||
        (!accel_available && !gyro_available)) {
        return false;
    }

    if (accel_available) {
        float raw_x, raw_y, raw_z;
        if (bmi270_bmm150_sensor_read_acceleration(
                s_sensor, &raw_x, &raw_y, &raw_z) != ESP_OK) {
            return false;
        }
        // The component reports g. M5Stack's official StopWatch HAL swaps the
        // BMI270 X/Y channels to align them with the display coordinate frame.
        s_accel[0] = raw_y * G_TO_MPS2;
        s_accel[1] = raw_x * G_TO_MPS2;
        s_accel[2] = raw_z * G_TO_MPS2;
        memcpy(s_raw_accel, s_accel, sizeof(s_raw_accel));
    }

    if (gyro_available) {
        float raw_x, raw_y, raw_z;
        if (bmi270_bmm150_sensor_read_gyroscope(
                s_sensor, &raw_x, &raw_y, &raw_z) != ESP_OK) {
            return false;
        }
        s_gyro[0] = raw_y;
        s_gyro[1] = raw_x;
        s_gyro[2] = raw_z;
    }

    const float ax = IMU_MAP_X(s_accel[0], s_accel[1], s_accel[2]);
    const float ay = IMU_MAP_Y(s_accel[0], s_accel[1], s_accel[2]);
    const float az = IMU_MAP_Z(s_accel[0], s_accel[1], s_accel[2]);
    const float gx = IMU_MAP_X(s_gyro[0], s_gyro[1], s_gyro[2]) * DEG_TO_RAD;
    const float gy = IMU_MAP_Y(s_gyro[0], s_gyro[1], s_gyro[2]) * DEG_TO_RAD;
    const float gz = IMU_MAP_Z(s_gyro[0], s_gyro[1], s_gyro[2]) * DEG_TO_RAD;

    if (!s_lp_primed) {
        s_lp[0] = ax;
        s_lp[1] = ay;
        s_lp[2] = az;
        s_lp_primed = true;
    } else {
        const float k = 1.0f - expf(-TWO_PI * GRAVITY_LP_HZ * dt);
        s_lp[0] += k * (ax - s_lp[0]);
        s_lp[1] += k * (ay - s_lp[1]);
        s_lp[2] += k * (az - s_lp[2]);
    }

    float down_x = -s_lp[0];
    float down_y = -s_lp[1];
    float down_z = -s_lp[2];
    const float magnitude =
        sqrtf(down_x * down_x + down_y * down_y + down_z * down_z);
    if (magnitude > 0.5f) {
        down_x /= magnitude;
        down_y /= magnitude;
        down_z /= magnitude;
    } else {
        down_x = 0.0f;
        down_y = 1.0f;
        down_z = 0.0f;
    }

    const float gravity_px =
        GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER;
    const float shake_scale = SHAKE_GAIN * PX_PER_METER;
    out->gravity[0] = down_x * gravity_px - (ax - s_lp[0]) * shake_scale;
    out->gravity[1] = down_y * gravity_px - (ay - s_lp[1]) * shake_scale;
    out->gravity[2] = down_z * gravity_px - (az - s_lp[2]) * shake_scale;
    out->down[0] = down_x;
    out->down[1] = down_y;
    out->down[2] = down_z;

    out->omega[0] = gx;
    out->omega[1] = gy;
    out->omega[2] = gz;

    const float inv_dt = dt > 1e-6f ? 1.0f / dt : 0.0f;
    out->alpha[0] = (gx - s_prev_omega[0]) * inv_dt;
    out->alpha[1] = (gy - s_prev_omega[1]) * inv_dt;
    out->alpha[2] = (gz - s_prev_omega[2]) * inv_dt;
    s_prev_omega[0] = gx;
    s_prev_omega[1] = gy;
    s_prev_omega[2] = gz;
    return true;
}
