#include "battery.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define M5PM1_ADDRESS 0x6E
#define M5PM1_GPIO_MODE_REGISTER 0x10
#define M5PM1_GPIO_INPUT_REGISTER 0x12
#define M5PM1_GPIO_PULL_REGISTER 0x14
#define M5PM1_GPIO_FUNCTION_REGISTER 0x16
#define M5PM1_CHARGE_STATUS_GPIO 2
#define M5PM1_VBAT_LOW_REGISTER 0x22
#define M5PM1_VIN_LOW_REGISTER 0x24
#define M5PM1_I2C_SPEED_HZ 100000
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200
#define EXTERNAL_POWER_PRESENT_MV 4000

static const char *TAG = "battery";
static i2c_master_dev_handle_t s_device;
static bool s_charge_status_ready;

static esp_err_t read_vbat_raw(uint8_t raw[2])
{
    const uint8_t reg = M5PM1_VBAT_LOW_REGISTER;
    return i2c_master_transmit_receive(s_device, &reg, sizeof(reg), raw, 2, 100);
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_device, &reg, sizeof(reg), value, 1, 100);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[2] = {reg, value};
    return i2c_master_transmit(s_device, bytes, sizeof(bytes), 100);
}

static esp_err_t configure_charge_status_input(void)
{
    uint8_t value = 0;
    esp_err_t err = read_register(M5PM1_GPIO_FUNCTION_REGISTER, &value);
    if (err != ESP_OK) return err;
    value &= (uint8_t)~(0x03U << (M5PM1_CHARGE_STATUS_GPIO * 2));
    err = write_register(M5PM1_GPIO_FUNCTION_REGISTER, value);
    if (err != ESP_OK) return err;

    err = read_register(M5PM1_GPIO_MODE_REGISTER, &value);
    if (err != ESP_OK) return err;
    value &= (uint8_t)~(1U << M5PM1_CHARGE_STATUS_GPIO);
    err = write_register(M5PM1_GPIO_MODE_REGISTER, value);
    if (err != ESP_OK) return err;

    err = read_register(M5PM1_GPIO_PULL_REGISTER, &value);
    if (err != ESP_OK) return err;
    value &= (uint8_t)~(0x03U << (M5PM1_CHARGE_STATUS_GPIO * 2));
    return write_register(M5PM1_GPIO_PULL_REGISTER, value);
}

esp_err_t battery_init(i2c_bus_handle_t bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    if (s_device != NULL) return ESP_OK;
    i2c_master_bus_handle_t master = i2c_bus_get_internal_bus_handle(bus);
    if (master == NULL) return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5PM1_ADDRESS,
        .scl_speed_hz = M5PM1_I2C_SPEED_HZ,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = false},
    };
    esp_err_t err = i2c_master_bus_add_device(master, &config, &s_device);
    if (err != ESP_OK) return err;

    // M5PM1 may be asleep after reset.  The first transaction generates its
    // wake condition; wait briefly, then perform the validated measurement.
    uint8_t wake_raw[2] = {0, 0};
    (void)read_vbat_raw(wake_raw);
    vTaskDelay(pdMS_TO_TICKS(10));

    err = configure_charge_status_input();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1 CHG_STAT unavailable: %s", esp_err_to_name(err));
    } else {
        s_charge_status_ready = true;
    }

    uint8_t percent = 0;
    uint16_t millivolts = 0;
    err = battery_read(&percent, &millivolts);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        ESP_LOGW(TAG, "M5PM1 VBAT unavailable: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "M5PM1 battery ready: %u mV, %u%%",
             (unsigned)millivolts, (unsigned)percent);
    return ESP_OK;
}

esp_err_t battery_read(uint8_t *percent, uint16_t *millivolts)
{
    if (percent == NULL || millivolts == NULL) return ESP_ERR_INVALID_ARG;
    if (s_device == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t raw[2] = {0, 0};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = read_vbat_raw(raw);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 20 : 100));
    }
    if (err != ESP_OK) return err;
    const uint16_t mv = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
    if (mv < 2500 || mv > 5000) {
        ESP_LOGW(TAG, "M5PM1 returned implausible VBAT: %u mV", (unsigned)mv);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t level;
    if (mv <= BATTERY_EMPTY_MV) level = 0;
    else if (mv >= BATTERY_FULL_MV) level = 100;
    else level = (uint8_t)(((uint32_t)(mv - BATTERY_EMPTY_MV) * 100U) /
                           (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
    *millivolts = mv;
    *percent = level;
    return ESP_OK;
}

esp_err_t battery_is_charging(bool *charging)
{
    if (charging == NULL) return ESP_ERR_INVALID_ARG;
    if (s_device == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_charge_status_ready) return ESP_ERR_INVALID_STATE;

    const uint8_t vin_reg = M5PM1_VIN_LOW_REGISTER;
    uint8_t vin_raw[2] = {0, 0};
    esp_err_t err = i2c_master_transmit_receive(s_device, &vin_reg,
                                                sizeof(vin_reg), vin_raw,
                                                sizeof(vin_raw), 100);
    if (err != ESP_OK) return err;
    uint8_t gpio_input = 0;
    err = read_register(M5PM1_GPIO_INPUT_REGISTER, &gpio_input);
    if (err != ESP_OK) return err;

    const uint16_t vin_mv = (uint16_t)vin_raw[0] | ((uint16_t)vin_raw[1] << 8);
    const bool external_power = vin_mv > EXTERNAL_POWER_PRESENT_MV;
    const bool charge_active =
        (gpio_input & (1U << M5PM1_CHARGE_STATUS_GPIO)) == 0;
    *charging = external_power && charge_active;
    ESP_LOGI(TAG, "charge status: VIN=%u mV CHG_STAT=%u charging=%d",
             (unsigned)vin_mv, charge_active ? 0U : 1U, *charging);
    return ESP_OK;
}
