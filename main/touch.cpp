#include "touch.h"

#include <cmath>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr uint8_t kAddress = 0x15;
constexpr uint8_t kRegStatus = 0x00;
constexpr uint8_t kRegChipId = 0xA7;
constexpr uint8_t kRegSoftVer = 0xA9;
constexpr int kSwipePixels = 60;
constexpr int kLongPressMovePixels = 30;
constexpr int64_t kTapMaxUs = 450000;
constexpr int64_t kDoubleTapUs = 380000;
constexpr int64_t kLongPressUs = 700000;

i2c_master_dev_handle_t s_device;
bool s_pressed;
bool s_long_press_sent;
uint16_t s_start_x, s_start_y, s_last_x, s_last_y;
int64_t s_down_us;
bool s_pending_tap;
uint16_t s_tap_x, s_tap_y;
int64_t s_tap_us;
int64_t s_raw_last_contact_us;
uint16_t s_raw_last_x, s_raw_last_y;
int64_t s_raw_last_log_us;
const char *TAG = "touch";

bool read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, data, len, 20) == ESP_OK;
}

bool read_point(bool *pressed, uint16_t *x, uint16_t *y)
{
    uint8_t data[7];
    if (!read_reg(kRegStatus, data, sizeof(data))) return false;
    const uint8_t fingers = data[2] & 0x0F;
    *x = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
    *y = ((uint16_t)(data[5] & 0x0F) << 8) | data[6];
    // Match M5Stack's official StopWatch input path: finger count is the
    // authoritative continuous-contact signal. Some CST820 revisions report
    // an event code other than Down/Contact while coordinates are moving;
    // filtering on those two codes turns a real drag into disconnected taps.
    *pressed = fingers > 0;
    return true;
}

}  // namespace

extern "C" esp_err_t touch_init(i2c_bus_handle_t bus)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(board_touch_reset(), TAG, "touch reset failed");

    i2c_master_bus_handle_t native = i2c_bus_get_internal_bus_handle(bus);
    if (native == nullptr) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(native, &config, &s_device),
                        TAG, "add CST820 failed");

    uint8_t chip = 0, version = 0;
    if (!read_reg(kRegChipId, &chip, 1) || !read_reg(kRegSoftVer, &version, 1) ||
        chip == 0 || version == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "CST820 ready: chip 0x%02x, version 0x%02x", chip, version);
    return ESP_OK;
}

extern "C" touch_event_t touch_poll(uint16_t *x, uint16_t *y)
{
    const int64_t now = esp_timer_get_time();
    bool pressed;
    uint16_t px, py;
    if (s_device == nullptr || !read_point(&pressed, &px, &py)) return TOUCH_EVENT_NONE;

    if (pressed && !s_pressed) {
        s_pressed = true;
        s_long_press_sent = false;
        s_start_x = s_last_x = px;
        s_start_y = s_last_y = py;
        s_down_us = now;
    } else if (pressed) {
        s_last_x = px;
        s_last_y = py;
        const int dx = (int)s_last_x - (int)s_start_x;
        const int dy = (int)s_last_y - (int)s_start_y;
        if (!s_long_press_sent && now - s_down_us >= kLongPressUs &&
            std::abs(dx) < kLongPressMovePixels &&
            std::abs(dy) < kLongPressMovePixels) {
            s_long_press_sent = true;
            s_pending_tap = false;
            if (x) *x = s_last_x;
            if (y) *y = s_last_y;
            return TOUCH_EVENT_LONG_PRESS;
        }
    } else if (s_pressed) {
        s_pressed = false;
        if (s_long_press_sent) return TOUCH_EVENT_NONE;
        const int dx = (int)s_last_x - (int)s_start_x;
        const int dy = (int)s_last_y - (int)s_start_y;
        if (std::abs(dx) >= kSwipePixels || std::abs(dy) >= kSwipePixels) {
            s_pending_tap = false;
            if (std::abs(dx) > std::abs(dy)) {
                return dx > 0 ? TOUCH_EVENT_SWIPE_RIGHT : TOUCH_EVENT_SWIPE_LEFT;
            }
            return dy > 0 ? TOUCH_EVENT_SWIPE_DOWN : TOUCH_EVENT_SWIPE_UP;
        }
        if (now - s_down_us <= kTapMaxUs) {
            if (s_pending_tap && now - s_tap_us <= kDoubleTapUs) {
                s_pending_tap = false;
                if (x) *x = s_last_x;
                if (y) *y = s_last_y;
                return TOUCH_EVENT_DOUBLE_TAP;
            }
            s_pending_tap = true;
            s_tap_x = s_last_x;
            s_tap_y = s_last_y;
            s_tap_us = now;
        }
    }

    if (s_pending_tap && !s_pressed && now - s_tap_us > kDoubleTapUs) {
        s_pending_tap = false;
        if (x) *x = s_tap_x;
        if (y) *y = s_tap_y;
        return TOUCH_EVENT_TAP;
    }
    return TOUCH_EVENT_NONE;
}

extern "C" esp_err_t touch_read_raw(bool *pressed, uint16_t *x, uint16_t *y)
{
    if (!pressed || !x || !y) return ESP_ERR_INVALID_ARG;
    if (s_device == nullptr) return ESP_ERR_INVALID_STATE;
    bool contact = false;
    uint16_t px = 0, py = 0;
    if (!read_point(&contact, &px, &py)) return ESP_FAIL;
    const int64_t now = esp_timer_get_time();
    if (contact) {
        s_raw_last_contact_us = now;
        s_raw_last_x = px;
        s_raw_last_y = py;
        *pressed = true;
        *x = px;
        *y = py;
        if (now - s_raw_last_log_us >= 100000) {
            s_raw_last_log_us = now;
            ESP_LOGI(TAG, "selector raw contact: %u,%u", (unsigned)px,
                     (unsigned)py);
        }
    } else if (s_raw_last_contact_us != 0 &&
               now - s_raw_last_contact_us < 120000) {
        // CST820 may clear its contact packet between coordinate updates.
        // Bridge those short gaps so a circular drag remains one continuous
        // gesture instead of repeatedly becoming press/release pairs.
        *pressed = true;
        *x = s_raw_last_x;
        *y = s_raw_last_y;
    } else {
        *pressed = false;
        *x = s_raw_last_x;
        *y = s_raw_last_y;
    }
    return ESP_OK;
}

extern "C" void touch_cancel_gestures(void)
{
    s_pressed = false;
    s_long_press_sent = false;
    s_pending_tap = false;
    s_raw_last_contact_us = 0;
}
