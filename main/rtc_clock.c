#include "rtc_clock.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define RX8130_ADDRESS 0x32
#define RX8130_TIME_REG 0x10
#define RX8130_STATUS_REG 0x1D
#define RX8130_VOLTAGE_LOW 0x80

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_device;

static bool valid_bcd(uint8_t value)
{
    return (value & 0x0F) <= 9 && ((value >> 4) & 0x0F) <= 9;
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_device == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_device, &reg, 1, data, length, 30);
}

static uint8_t to_bcd(int value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static esp_err_t write_register(uint8_t reg, const uint8_t *data, size_t length)
{
    if (s_device == NULL || data == NULL || length > 15) return ESP_ERR_INVALID_STATE;
    uint8_t payload[16];
    payload[0] = reg;
    memcpy(payload + 1, data, length);
    return i2c_master_transmit(s_device, payload, length + 1, 30);
}

esp_err_t rtc_clock_init(i2c_bus_handle_t bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_handle_t native = i2c_bus_get_internal_bus_handle(bus);
    if (native == NULL) return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RX8130_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(native, &config, &s_device),
                        TAG, "add RX8130CE failed");

    uint8_t hours = 0, minutes = 0;
    ESP_RETURN_ON_ERROR(rtc_clock_read(&hours, &minutes), TAG,
                        "RX8130CE time invalid");

    uint8_t status = 0;
    if (read_register(RX8130_STATUS_REG, &status, 1) == ESP_OK &&
        (status & RX8130_VOLTAGE_LOW)) {
        ESP_LOGW(TAG, "RX8130CE voltage-low flag is set; time may need setting");
    }
    ESP_LOGI(TAG, "RX8130CE ready: %02u:%02u", hours, minutes);
    return ESP_OK;
}

esp_err_t rtc_clock_read(uint8_t *hours, uint8_t *minutes)
{
    uint8_t seconds = 0;
    return rtc_clock_read_hms(hours, minutes, &seconds);
}

esp_err_t rtc_clock_read_hms(uint8_t *hours, uint8_t *minutes, uint8_t *seconds)
{
    if (hours == NULL || minutes == NULL || seconds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t time[3];
    ESP_RETURN_ON_ERROR(read_register(RX8130_TIME_REG, time, sizeof(time)), TAG,
                        "read time failed");

    const uint8_t second_bcd = time[0] & 0x7F;
    const uint8_t minute_bcd = time[1] & 0x7F;
    const uint8_t hour_bcd = time[2] & 0x3F;
    if (!valid_bcd(second_bcd) || !valid_bcd(minute_bcd) ||
        !valid_bcd(hour_bcd)) return ESP_ERR_INVALID_RESPONSE;

    const uint8_t decoded_seconds = from_bcd(second_bcd);
    const uint8_t decoded_minutes = from_bcd(minute_bcd);
    const uint8_t decoded_hours = from_bcd(hour_bcd);
    if (decoded_seconds > 59 || decoded_minutes > 59 || decoded_hours > 23) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *hours = decoded_hours;
    *minutes = decoded_minutes;
    *seconds = decoded_seconds;
    return ESP_OK;
}

esp_err_t rtc_clock_set_unix(int64_t unix_seconds, int16_t timezone_minutes)
{
    if (unix_seconds < 946684800LL || unix_seconds > 4102444799LL) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t local_epoch = (time_t)(unix_seconds + (int64_t)timezone_minutes * 60);
    struct tm local_time;
    if (gmtime_r(&local_epoch, &local_time) == NULL) return ESP_FAIL;
    const int year = local_time.tm_year + 1900;
    if (year < 2000 || year > 2099) return ESP_ERR_INVALID_ARG;
    const uint8_t values[7] = {
        to_bcd(local_time.tm_sec), to_bcd(local_time.tm_min),
        to_bcd(local_time.tm_hour), (uint8_t)(1U << local_time.tm_wday),
        to_bcd(local_time.tm_mday), to_bcd(local_time.tm_mon + 1),
        to_bcd(year - 2000),
    };
    const esp_err_t err = write_register(RX8130_TIME_REG, values, sizeof(values));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "calibrated from phone: %04d-%02d-%02d %02d:%02d:%02d UTC%+d:%02d",
                 year, local_time.tm_mon + 1, local_time.tm_mday,
                 local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                 timezone_minutes / 60, abs(timezone_minutes % 60));
    }
    return err;
}
