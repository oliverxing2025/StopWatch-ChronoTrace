#include "board.h"

#include <M5IOE1.h>

#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint8_t kPrimaryAddress = 0x4F;
constexpr uint8_t kSecondaryAddress = 0x6F;
constexpr uint32_t kI2cSpeedHz = M5IOE1_I2C_FREQ_400K;

// StopWatch wiring from the official M5Stack HAL.
constexpr uint8_t kDisplayRailPin = M5IOE1_PIN_8;
constexpr uint8_t kDisplayResetPin = M5IOE1_PIN_5;
constexpr uint8_t kAudioEnablePin = M5IOE1_PIN_3;
constexpr uint8_t kSpeakerPaPin = M5IOE1_PIN_10;
constexpr uint8_t kTouchResetPin = M5IOE1_PIN_4;
constexpr uint8_t kMotorPin = M5IOE1_PIN_9;
constexpr uint8_t kMotorPwmChannel = M5IOE1_PWM_CH1;  // IO9 on StopWatch
constexpr uint16_t kMotorPwmHz = 2000;
constexpr uint8_t kHapticDutyPercent = 32;
constexpr TickType_t kHapticDuration = pdMS_TO_TICKS(28);
constexpr gpio_num_t kSpeakerPaGpio = GPIO_NUM_14;

M5IOE1 s_ioe;
TaskHandle_t s_haptic_task;
const char *TAG = "board";

void haptic_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const m5ioe1_err_t start_err = s_ioe.setPwmDuty(
            kMotorPwmChannel, kHapticDutyPercent, false, true);
        if (start_err != M5IOE1_OK) {
            ESP_LOGW(TAG, "haptic start failed: %d", (int)start_err);
            continue;
        }

        // A new tap during the pulse restarts its short deadline. All M5IOE1
        // transactions stay in this task; FreeRTOS' small Timer Service stack
        // must never perform these comparatively heavy I2C calls.
        while (ulTaskNotifyTake(pdTRUE, kHapticDuration) > 0) {
        }

        const m5ioe1_err_t stop_err = s_ioe.setPwmDuty(
            kMotorPwmChannel, 0, false, false);
        if (stop_err != M5IOE1_OK) {
            ESP_LOGW(TAG, "haptic stop failed: %d", (int)stop_err);
        }
    }
}

esp_err_t set_output(uint8_t pin, uint8_t level)
{
    m5ioe1_err_t err = M5IOE1_OK;
    s_ioe.pinModeWithRes(pin, OUTPUT, &err);
    if (err != M5IOE1_OK) {
        return ESP_FAIL;
    }
    s_ioe.digitalWriteWithRes(pin, level, &err);
    return err == M5IOE1_OK ? ESP_OK : ESP_FAIL;
}

}  // namespace

extern "C" esp_err_t board_power_init(i2c_bus_handle_t i2c_bus)
{
    if (i2c_bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t address = kPrimaryAddress;
    m5ioe1_err_t ioe_err = s_ioe.begin(i2c_bus, address, kI2cSpeedHz,
                                       M5IOE1_INT_MODE_DISABLED);
    if (ioe_err != M5IOE1_OK) {
        address = kSecondaryAddress;
        ioe_err = s_ioe.begin(i2c_bus, address, kI2cSpeedHz,
                              M5IOE1_INT_MODE_DISABLED);
    }
    if (ioe_err != M5IOE1_OK) {
        ESP_LOGE(TAG, "M5IOE1 not found at 0x4f or 0x6f");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(set_output(kDisplayRailPin, 1), TAG,
                        "display power rail enable failed");
    ESP_RETURN_ON_ERROR(set_output(kAudioEnablePin, 1), TAG,
                        "audio rail enable failed");
    ESP_RETURN_ON_ERROR(set_output(kSpeakerPaPin, 0), TAG,
                        "speaker amp mute failed");
    ESP_RETURN_ON_ERROR(set_output(kTouchResetPin, 1), TAG,
                        "touch reset release failed");
    // Match M5Stack's official StopWatch motor configuration. Keeping PWM
    // disabled at boot prevents a stale expander state from vibrating.
    // Establish push-pull drive before handing IO9 to PWM. Merely setting the
    // PWM enable bit leaves the expander's existing drive mode untouched.
    if (set_output(kMotorPin, 0) != ESP_OK ||
        s_ioe.setPwmFrequency(kMotorPwmHz) != M5IOE1_OK ||
        s_ioe.setPwmDuty(kMotorPwmChannel, 0, false, false) != M5IOE1_OK) {
        ESP_LOGW(TAG, "vibration motor initialization failed");
    } else {
        if (xTaskCreatePinnedToCore(haptic_task, "haptic", 3072, nullptr, 5,
                                    &s_haptic_task, 0) != pdPASS) {
            s_haptic_task = nullptr;
            ESP_LOGW(TAG, "haptic task allocation failed");
        }
    }
    gpio_set_direction(kSpeakerPaGpio, GPIO_MODE_OUTPUT);
    gpio_set_level(kSpeakerPaGpio, 0);
    ESP_RETURN_ON_ERROR(set_output(kDisplayResetPin, 0), TAG,
                        "display reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(set_output(kDisplayResetPin, 1), TAG,
                        "display reset release failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "display power ready through M5IOE1 at 0x%02x", address);
    return ESP_OK;
}

extern "C" esp_err_t board_speaker_enable(bool enable)
{
    if (enable) {
        ESP_RETURN_ON_ERROR(gpio_set_level(kSpeakerPaGpio, 1), TAG,
                            "speaker GPIO enable failed");
        const m5ioe1_err_t err = s_ioe.setAw8737aMode(
            kSpeakerPaPin, M5IOE1_AW8737A_MODE_3, M5IOE1_AW8737A_REFRESH_NOW);
        if (err != M5IOE1_OK) {
            gpio_set_level(kSpeakerPaGpio, 0);
            ESP_LOGE(TAG, "AW8737A medium-gain setup failed: %d", (int)err);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "speaker path enabled: AW8737A medium gain, GPIO14 high");
    } else {
        const m5ioe1_err_t err = s_ioe.setAw8737aMode(
            kSpeakerPaPin, M5IOE1_AW8737A_MODE_1, M5IOE1_AW8737A_REFRESH_NOW);
        ESP_RETURN_ON_ERROR(gpio_set_level(kSpeakerPaGpio, 0), TAG,
                            "speaker GPIO mute failed");
        if (err != M5IOE1_OK) {
            ESP_LOGE(TAG, "AW8737A mute failed: %d", (int)err);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

extern "C" esp_err_t board_touch_reset(void)
{
    ESP_RETURN_ON_ERROR(set_output(kTouchResetPin, 0), TAG,
                        "touch reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(set_output(kTouchResetPin, 1), TAG,
                        "touch reset release failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

extern "C" void board_haptic_click(void)
{
    if (s_haptic_task != nullptr) xTaskNotifyGive(s_haptic_task);
}
