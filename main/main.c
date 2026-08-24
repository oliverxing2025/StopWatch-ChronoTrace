#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "audio.h"
#include "battery.h"
#include "ble_setup.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "imu.h"
#include "handwriting.h"
#include "render.h"
#include "rtc_clock.h"
#include "settings.h"
#include "sim.h"
#include "touch.h"
#include "wifi_setup.h"
#include "weather.h"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define SIM_YIELD_TICKS 1
#define BUTTON_PERIOD_MS 25
#define STATS_PERIOD_MS 2000

static const char *TAG = "chronotrace";
static i2c_bus_handle_t s_i2c_bus;
static volatile uint32_t s_frames;
static volatile uint32_t s_steps;
static chrono_trace_settings_t s_settings;
static bool s_audio_ready;
static bool s_battery_ready;
static bool s_touch_ready;
static float s_last_bass;
static bool s_clock_held;
static uint8_t s_clock_hours;
static uint8_t s_clock_minutes;
static uint8_t s_clock_seconds;
static bool s_clock_analog;
static bool s_clock_style_transition_pending;
static bool s_clock_style_transition_target_analog;
static int64_t s_clock_style_transition_deadline_us;
static bool s_random_shape_transition_pending;
static int64_t s_random_shape_transition_deadline_us;
static int64_t s_clock_last_poll_us;
static bool s_handwriting_playing;
static bool s_handwriting_restore_ready;
static uint8_t s_handwriting_index;
static int64_t s_handwriting_next_us;
static bool s_countdown_menu;
static int64_t s_countdown_menu_until_us;
static bool s_countdown_active;
static int64_t s_countdown_end_us;
static int64_t s_countdown_total_us;
static int64_t s_countdown_pause_remaining_us;
static int s_countdown_last_seconds = -1;
static bool s_countdown_paused;
static uint8_t s_countdown_minutes = 5;
static uint8_t s_countdown_drag_ring;
static bool s_countdown_touch_down;
static bool s_settings_open;
static uint8_t s_settings_page;
static bool s_operation_guide_open;
static uint8_t s_operation_guide_page;
static bool s_settings_touch_down;
static uint8_t s_settings_touch_target;
static uint16_t s_settings_touch_x;
static uint16_t s_settings_touch_y;
static uint16_t s_settings_touch_last_x;
static uint16_t s_settings_touch_last_y;
static uint8_t s_wifi_editor_mode;
static char s_wifi_ssids[6][33];
static size_t s_wifi_ssid_count;
static uint8_t s_wifi_selected;
static char s_wifi_password[65];
static bool s_wifi_uppercase;
static bool s_wifi_symbols;
static bool s_wifi_reveal;
static wifi_setup_state_t s_settings_wifi_state_seen = WIFI_SETUP_UNCONFIGURED;
static bool s_ble_time_calibrated;
static bool s_ble_time_calibrating;
static int64_t s_ble_calibration_deadline_us;
static ble_setup_state_t s_settings_ble_state_seen = BLE_SETUP_OFF;
static bool s_weather_open;
static bool s_weather_transition_pending;
static int64_t s_weather_transition_deadline_us;
static weather_state_t s_weather_state_seen = WEATHER_STATE_IDLE;

#define HANDWRITING_GLYPH_US 2800000
#define COUNTDOWN_MENU_US 120000000
#define COUNTDOWN_CENTER_X (LCD_H_RES / 2)
#define COUNTDOWN_CENTER_Y (LCD_V_RES / 2)

static const char *const s_theme_names[] = {
    "deep-sea", "cyber", "lava", "aurora", "mercury", "rainbow-prism",
    "gold", "diamond"
};
static const char *const s_theme_labels[] = {
    "主题：深海", "主题：赛博", "主题：熔岩", "主题：极光", "主题：水银", "主题：彩虹",
    "主题：黄金", "主题：钻石"
};
static const char *const s_theme_labels_en[] = {
    "Theme: Deep Sea", "Theme: Cyber", "Theme: Lava", "Theme: Aurora",
    "Theme: Mercury", "Theme: Rainbow", "Theme: Gold", "Theme: Diamond"
};
static const char *const s_density_labels[] = {
    "密度：少", "密度：中", "密度：多"
};
static const char *const s_density_labels_en[] = {
    "Density: Low", "Density: Medium", "Density: High"
};
static const char *ui_text(const char *chinese, const char *english)
{
    return s_settings.language ? english : chinese;
}

static void save_settings(void)
{
    const esp_err_t err = settings_save(&s_settings);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
}

static void set_theme(int delta)
{
    int theme = (int)s_settings.theme + delta;
    if (theme < 0) theme = 7;
    if (theme > 7) theme = 0;
    s_settings.theme = (uint8_t)theme;
    render_set_theme(s_settings.theme);
    if (s_audio_ready) {
        audio_set_theme(s_settings.theme);
        audio_trigger(AUDIO_EVENT_THEME);
    }
    save_settings();
    render_show_message(s_settings.language ? s_theme_labels_en[s_settings.theme] :
                                               s_theme_labels[s_settings.theme]);
    ESP_LOGI(TAG, "theme: %s", s_theme_names[s_settings.theme]);
}

static void toggle_reactive(void)
{
    s_settings.reactive = !s_settings.reactive;
    render_set_reactive(s_settings.reactive);
    if (s_audio_ready) {
        audio_set_reactive(s_settings.reactive);
        audio_trigger(AUDIO_EVENT_REACTIVE);
    }
    save_settings();
    render_show_message(s_settings.reactive ?
                        ui_text("音乐律动：开启", "Music: On") :
                        ui_text("音乐律动：关闭", "Music: Off"));
    ESP_LOGI(TAG, "external music reactive mode: %s",
             s_settings.reactive ? "on" : "off");
}

static void adjust_main_volume(int direction)
{
    // The settings page remains a continuous 0..100 slider.  On the particle
    // canvas, vertical swipes deliberately keep the original three simple
    // listening levels so they are quick and predictable without looking.
    static const uint8_t levels[3] = {0, 38, 68};
    int level;
    if (s_settings.volume == 0) level = 0;
    else if (s_settings.volume < levels[2]) level = 1;
    else level = 2;
    level += direction > 0 ? 1 : -1;
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    s_settings.volume = levels[level];
    if (s_audio_ready) {
        audio_set_volume(s_settings.volume);
        if (s_settings.volume > 0) audio_trigger(AUDIO_EVENT_VOLUME);
    }
    save_settings();
    static const char *const labels_cn[3] = {"音量：静音", "音量：小", "音量：大"};
    static const char *const labels_en[3] = {"Volume: Mute", "Volume: Low", "Volume: High"};
    render_show_message(s_settings.language ? labels_en[level] : labels_cn[level]);
    ESP_LOGI(TAG, "touch volume level %d: %u", level, s_settings.volume);
}

static void start_handwriting_playback(bool show_message)
{
    if (handwriting_count() == 0) return;
    s_clock_held = false;
    s_handwriting_playing = true;
    s_handwriting_restore_ready = false;
    s_handwriting_index = 0;
    sim_show_handwriting(handwriting_glyph(0), HANDWRITING_W, HANDWRITING_H,
                         handwriting_glyph_color(0));
    s_handwriting_next_us = esp_timer_get_time() + HANDWRITING_GLYPH_US;
    if (show_message) render_show_message(ui_text("笔迹播放", "Ink playback"));
}

static void set_countdown_menu(bool visible)
{
    s_countdown_menu = visible;
    s_countdown_menu_until_us = visible ? esp_timer_get_time() + COUNTDOWN_MENU_US : 0;
    s_countdown_touch_down = false;
    s_countdown_drag_ring = 0;
    render_set_countdown_menu(visible);
    touch_cancel_gestures();
    if (visible) {
        render_set_countdown_selector(s_countdown_minutes, false);
    }
}

static void start_countdown(int total_seconds)
{
    if (total_seconds <= 0) return;
    set_countdown_menu(false);
    s_clock_held = false;
    s_handwriting_playing = false;
    s_handwriting_restore_ready = false;
    s_countdown_active = true;
    s_countdown_paused = false;
    s_countdown_pause_remaining_us = 0;
    s_countdown_total_us = (int64_t)total_seconds * 1000000;
    s_countdown_end_us = esp_timer_get_time() + s_countdown_total_us;
    s_countdown_last_seconds = -1;
    // The colourful dial is a selector only. Once confirmed, return to the
    // normal liquid scene and let the particles themselves form MM:SS while
    // the surrounding particle halo drains with the remaining ratio.
    render_set_countdown_runtime(false, 0, 0.0f, false);
    if (total_seconds >= 3600) {
        sim_show_countdown((uint8_t)(total_seconds / 3600),
                           (uint8_t)((total_seconds / 60) % 60), 1.0f);
    } else {
        sim_show_countdown((uint8_t)(total_seconds / 60), 0, 1.0f);
    }
    if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
    ESP_LOGI(TAG, "particle countdown started: %02d:%02d",
             total_seconds / 3600, (total_seconds / 60) % 60);
}

static void cancel_countdown(void)
{
    if (!s_countdown_active) return;
    s_countdown_active = false;
    s_countdown_paused = false;
    s_countdown_last_seconds = -1;
    render_set_countdown_runtime(false, 0, 0.0f, false);
    sim_release_formation();
    ESP_LOGI(TAG, "particle countdown canceled into fluid");
}

static void toggle_countdown_pause(void)
{
    if (!s_countdown_active) return;
    const int64_t now = esp_timer_get_time();
    if (s_countdown_paused) {
        s_countdown_end_us = now + s_countdown_pause_remaining_us;
        s_countdown_paused = false;
        ESP_LOGI(TAG, "countdown resumed");
    } else {
        s_countdown_pause_remaining_us = s_countdown_end_us - now;
        if (s_countdown_pause_remaining_us < 0) s_countdown_pause_remaining_us = 0;
        s_countdown_paused = true;
        ESP_LOGI(TAG, "countdown paused");
    }
    const float progress = s_countdown_total_us > 0 ?
        1.0f - (float)s_countdown_pause_remaining_us /
               (float)s_countdown_total_us : 1.0f;
    render_set_countdown_runtime(false, 0, progress, s_countdown_paused);
    if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
}

static uint8_t countdown_value_from_angle(uint16_t x, uint16_t y, int divisions)
{
    const float dx = (float)((int)x - COUNTDOWN_CENTER_X);
    const float dy = (float)((int)y - COUNTDOWN_CENTER_Y);
    float angle = atan2f(dx, -dy);
    if (angle < 0.0f) angle += 2.0f * (float)M_PI;
    int value = (int)lroundf(angle * divisions / (2.0f * (float)M_PI));
    if (value > divisions) value = divisions;
    // The outer duration dial is labelled 1..60 like a timer bezel. Twelve
    // o'clock therefore means 60, not an unexplained 00.
    if (divisions == 60 && value == 0) value = 60;
    return (uint8_t)value;
}

static void poll_countdown_selector(void)
{
    bool pressed = false;
    uint16_t x = 0, y = 0;
    if (touch_read_raw(&pressed, &x, &y) != ESP_OK) return;
    const int start_dx = (int)x - COUNTDOWN_CENTER_X;
    const int start_dy = (int)y - 334;
    const int start_distance2 = start_dx * start_dx + start_dy * start_dy;

    if (pressed) {
        s_countdown_menu_until_us = esp_timer_get_time() + COUNTDOWN_MENU_US;
        if (!s_countdown_touch_down) {
            s_countdown_touch_down = true;
            if (start_distance2 <= 52 * 52) {
                s_countdown_drag_ring = 3;
            } else {
                // Scheme 2 deliberately makes the whole non-central dial a
                // drag target; the user never has to catch the small handle.
                s_countdown_drag_ring = 1;
            }
            ESP_LOGI(TAG, "countdown touch down: %u,%u target=%u",
                     (unsigned)x, (unsigned)y, (unsigned)s_countdown_drag_ring);
        }
        if (s_countdown_drag_ring == 1) {
            const uint8_t selected = countdown_value_from_angle(x, y, 60);
            if (selected != s_countdown_minutes) {
                s_countdown_minutes = selected;
                ESP_LOGI(TAG, "countdown drag: %u,%u -> %u min",
                         (unsigned)x, (unsigned)y,
                         (unsigned)s_countdown_minutes);
            }
        }
        render_set_countdown_selector(s_countdown_minutes,
                                       s_countdown_drag_ring == 1);
    } else if (s_countdown_touch_down) {
        const uint8_t released_ring = s_countdown_drag_ring;
        s_countdown_touch_down = false;
        s_countdown_drag_ring = 0;
        if (released_ring == 3) {
            const int total_seconds =
                (int)s_countdown_minutes * 60;
            if (total_seconds > 0) start_countdown(total_seconds);
        } else if (released_ring != 0 && s_audio_ready) {
            audio_trigger(AUDIO_EVENT_TOUCH);
        }
        render_set_countdown_selector(s_countdown_minutes, false);
        if (released_ring == 1) {
            ESP_LOGI(TAG, "countdown dial selected: %u minutes",
                     (unsigned)s_countdown_minutes);
        }
    }
}

static void refresh_settings_screen(void)
{
    const bool connected = ble_setup_state() == BLE_SETUP_CONNECTED;
    weather_snapshot_t weather;
    weather_snapshot(&weather);
    render_set_settings(s_settings_open, s_settings.language,
                        s_settings.bluetooth_enabled, connected,
                        connected && s_ble_time_calibrating,
                        connected && s_ble_time_calibrated,
                        s_settings.volume, s_settings.brightness,
                        s_settings.haptic_enabled, s_settings_page,
                        s_settings.wifi_enabled,
                        (uint8_t)wifi_setup_state(),
                        weather.automatic_location);
}

static void refresh_weather_screen(void)
{
    weather_snapshot_t weather;
    weather_snapshot(&weather);
    // Weather is formed by the real liquid simulation. Disable the former
    // text card so the page contains only the particle icon and temperature.
    render_set_weather(false, s_settings.language,
                       (uint8_t)weather.state, weather.valid, weather.city,
                       weather.temperature_c, weather.apparent_c,
                       weather.humidity, weather.wind_kmh,
                       weather.weather_code, weather.updated_unix);
    if (s_weather_open && !s_weather_transition_pending) {
        sim_show_weather((int16_t)lroundf(weather.minimum_c),
                         (int16_t)lroundf(weather.maximum_c),
                         weather.weather_code, weather.valid);
    }
}

static void close_weather_screen(void)
{
    s_weather_open = false;
    s_weather_transition_pending = false;
    refresh_weather_screen();
    if (s_clock_held) {
        uint8_t seconds = s_clock_seconds;
        (void)rtc_clock_read_hms(&s_clock_hours, &s_clock_minutes, &seconds);
        s_clock_seconds = seconds;
        if (s_clock_analog) {
            sim_hold_analog_time(s_clock_hours, s_clock_minutes, s_clock_seconds);
        } else {
            sim_hold_time(s_clock_hours, s_clock_minutes);
        }
    } else sim_release_formation();
}

static void refresh_wifi_editor(void)
{
    render_set_wifi_editor(s_wifi_editor_mode, s_settings.language,
                           s_wifi_ssids, (uint8_t)s_wifi_ssid_count,
                           s_wifi_selected, s_wifi_password,
                           s_wifi_uppercase, s_wifi_symbols, s_wifi_reveal);
}

static void close_wifi_editor(void)
{
    s_wifi_editor_mode = 0;
    memset(s_wifi_password, 0, sizeof(s_wifi_password));
    refresh_wifi_editor();
    refresh_settings_screen();
}

static void scan_wifi_for_editor(void)
{
    s_wifi_editor_mode = 1;
    s_wifi_ssid_count = 0;
    s_wifi_selected = 0;
    memset(s_wifi_ssids, 0, sizeof(s_wifi_ssids));
    refresh_wifi_editor();
    const esp_err_t err = wifi_setup_scan(s_wifi_ssids, 6, &s_wifi_ssid_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        render_show_message(ui_text("Wi-Fi 扫描失败", "Wi-Fi scan failed"));
    }
    refresh_wifi_editor();
}

static void set_settings_open(bool open)
{
    s_settings_open = open;
    if (!open) {
        close_wifi_editor();
        s_operation_guide_open = false;
        render_set_operation_guide(false, s_settings.language, 0);
    }
    if (open) {
        s_weather_open = false;
        s_weather_transition_pending = false;
        s_clock_style_transition_pending = false;
        s_random_shape_transition_pending = false;
        refresh_weather_screen();
        s_settings_page = 0;
        set_countdown_menu(false);
        if (handwriting_active()) handwriting_cancel();
        touch_cancel_gestures();
    }
    s_settings_touch_down = false;
    s_settings_touch_target = 0;
    refresh_settings_screen();
}

static void set_operation_guide(bool open, uint8_t page)
{
    s_operation_guide_open = open;
    s_operation_guide_page = page ? 1 : 0;
    s_settings_touch_down = false;
    s_settings_touch_target = 0;
    render_set_operation_guide(open, s_settings.language,
                               s_operation_guide_page);
    ESP_LOGI(TAG, "operation guide %s, page=%s",
             open ? "open" : "closed",
             s_operation_guide_page ? "touch" : "buttons");
}

static uint8_t panel_brightness(uint8_t percent);

static int settings_choice_from_x(uint16_t x, int count)
{
    if (x < 198 || x > 412) return -1;
    if (count == 2) return x < 305 ? 0 : 1;
    if (x < 270) return 0;
    if (x < 341) return 1;
    return 2;
}

static void settings_click_feedback(void)
{
    if (s_settings.haptic_enabled) board_haptic_click();
    if (s_audio_ready && s_settings.volume > 0) {
        audio_trigger(AUDIO_EVENT_UI_CLICK);
    }
}

static void handle_settings_tap(uint16_t x, uint16_t y)
{
    if (y >= 322 && y <= 392) {
        if (x <= 228) {
            settings_click_feedback();
            set_settings_open(false);
            ESP_LOGI(TAG, "settings exit: returned to particle scene");
        } else if (x >= 238) {
            settings_click_feedback();
            set_operation_guide(true, 0);
        }
        return;
    }
    const int first_top = s_settings_page == 0 ? 78 : 86;
    const int pitch = s_settings_page == 0 ? 58 : 72;
    const int row_height = s_settings_page == 0 ? 48 : 56;
    const int row_count = s_settings_page == 0 ? 4 : 3;
    if (y < first_top || y > first_top + (row_count - 1) * pitch + row_height) return;
    const int offset = (int)y - first_top;
    const int row = offset / pitch;
    if (offset % pitch > row_height || row < 0 || row >= row_count) return;
    bool changed = false;
    if (s_settings_page == 0 && row == 0) {
        const int choice = settings_choice_from_x(x, 2);
        if (choice < 0) return;
        settings_click_feedback();
        if (choice == 0) {
            const bool enable = !s_settings.wifi_enabled;
            const esp_err_t err = wifi_setup_set_enabled(enable);
            if (err == ESP_OK) {
                s_settings.wifi_enabled = enable;
                save_settings();
                if (enable && !wifi_setup_has_credentials()) {
                    scan_wifi_for_editor();
                    return;
                }
            } else {
                render_show_message(ui_text("Wi-Fi 操作失败", "Wi-Fi action failed"));
            }
            refresh_settings_screen();
        } else {
            // The status button remains an entry point even while connected,
            // so the user can select and join a different network.
            scan_wifi_for_editor();
        }
        return;
    } else if (s_settings_page == 0 && row == 1) {
        const int choice = settings_choice_from_x(x, 2);
        if (choice < 0) return;
        settings_click_feedback();
        if (choice == 0) {
            const bool enable = !s_settings.bluetooth_enabled;
            const esp_err_t err = ble_setup_set_enabled(enable);
            if (err == ESP_OK) {
                s_settings.bluetooth_enabled = enable;
                s_ble_time_calibrated = false;
                s_ble_time_calibrating = false;
                if (enable) {
                    render_show_message(ui_text("等待手机连接", "Waiting for phone"));
                }
                changed = true;
            } else {
                render_show_message(ui_text("蓝牙操作失败", "Bluetooth action failed"));
            }
        } else if (choice == 1) {
            if (ble_setup_state() != BLE_SETUP_CONNECTED) {
                render_show_message(ui_text("请先连接", "Connect first"));
            } else if (s_ble_time_calibrating) {
                // Keep the in-flight Current Time Service request singular.
                // A second tap while pairing/discovery is active can otherwise
                // restart discovery and make the state appear to jump.
                return;
            } else {
                s_ble_time_calibrated = false;
                const esp_err_t err = ble_setup_request_time();
                if (err == ESP_OK) {
                    s_ble_time_calibrating = true;
                    s_ble_calibration_deadline_us =
                        esp_timer_get_time() + 15000000;
                    render_show_message(ui_text("校准中", "Syncing"));
                    ESP_LOGI(TAG, "phone time calibration requested");
                } else {
                    s_ble_time_calibrating = false;
                    render_show_message(ui_text("校准失败", "Calibration failed"));
                }
            }
        }
    } else if (s_settings_page == 0 && row == 2) {
        const int choice = settings_choice_from_x(x, 2);
        if (choice < 0) return;
        settings_click_feedback();
        if (choice == 0) {
            weather_use_automatic_location();
            render_show_message(ui_text("自动定位中", "Locating"));
            refresh_settings_screen();
        } else {
            weather_refresh();
            render_show_message(ui_text("天气刷新中", "Refreshing weather"));
        }
        return;
    } else if (s_settings_page == 0 && row == 3) {
        const int choice = settings_choice_from_x(x, 2);
        if (choice < 0) return;
        settings_click_feedback();
        if (choice >= 0 && s_settings.language != (uint8_t)choice) {
            s_settings.language = (uint8_t)choice;
            render_set_language(s_settings.language);
            handwriting_set_language(s_settings.language);
            changed = true;
        }
    } else if (s_settings_page == 1 && row == 0) {
        static const uint8_t levels[3] = {68, 38, 0};
        const int choice = settings_choice_from_x(x, 3);
        if (choice < 0) return;
        if (s_settings.haptic_enabled) board_haptic_click();
        // Preserve an audible confirmation when selecting mute, and use the
        // newly selected level when moving out of mute.
        if (s_audio_ready && levels[choice] == 0 && s_settings.volume > 0) {
            audio_trigger(AUDIO_EVENT_UI_CLICK);
        }
        if (choice >= 0 && s_settings.volume != levels[choice]) {
            s_settings.volume = levels[choice];
            if (s_audio_ready) {
                audio_set_volume(s_settings.volume);
                if (s_settings.volume > 0) audio_trigger(AUDIO_EVENT_UI_CLICK);
            }
            changed = true;
        } else if (s_audio_ready && s_settings.volume > 0) {
            audio_trigger(AUDIO_EVENT_UI_CLICK);
        }
    } else if (s_settings_page == 1 && row == 1) {
        static const uint8_t levels[3] = {100, 55, 18};
        const int choice = settings_choice_from_x(x, 3);
        if (choice < 0) return;
        settings_click_feedback();
        if (choice >= 0 && s_settings.brightness != levels[choice]) {
            s_settings.brightness = levels[choice];
            const esp_err_t err = display_set_brightness(
                panel_brightness(s_settings.brightness));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "brightness update failed: %s", esp_err_to_name(err));
            }
            changed = true;
        }
    } else if (s_settings_page == 1 && row == 2) {
        const int choice = settings_choice_from_x(x, 2);
        if (choice < 0) return;
        const bool enable = choice == 0;
        if (s_settings.haptic_enabled) board_haptic_click();
        if (s_settings.haptic_enabled != enable) {
            s_settings.haptic_enabled = enable;
            if (enable) board_haptic_click();
            changed = true;
        }
        if (s_audio_ready && s_settings.volume > 0) {
            audio_trigger(AUDIO_EVENT_UI_CLICK);
        }
    }
    if (changed) save_settings();
    refresh_settings_screen();
}

static char wifi_key_at(uint16_t x, uint16_t y)
{
    static const char lower[4][11] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
    };
    static const char upper[4][11] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    static const char symbols[4][11] = {
        "1234567890", "!@#$%^&*()", "-_+=[]{}", ".,:;?/\\|"
    };
    if (y < 127 || y > 289) return '\0';
    const int row = ((int)y - 127) / 42;
    if (row < 0 || row > 3 || ((int)y - 127) % 42 > 36) return '\0';
    const int count = row < 3 ? (row == 2 ? 9 : 10) :
                      (s_wifi_symbols ? 8 : 7);
    const int width = count == 10 ? 36 : 38;
    const int gap = 3;
    const int total = count * width + (count - 1) * gap;
    const int start = (LCD_H_RES - total) / 2;
    if (x < start || x >= start + total) return '\0';
    const int index = ((int)x - start) / (width + gap);
    if (index >= count || ((int)x - start) % (width + gap) >= width) return '\0';
    const char (*rows)[11] = s_wifi_symbols ? symbols :
                              (s_wifi_uppercase ? upper : lower);
    return rows[row][index];
}

static void handle_wifi_editor_tap(uint16_t x, uint16_t y)
{
    if (s_wifi_editor_mode == 1) {
        if (y >= 392 && y <= 448) {
            if (x < LCD_H_RES / 2) close_wifi_editor();
            else scan_wifi_for_editor();
            return;
        }
        if (y >= 78) {
            const int row = ((int)y - 78) / 48;
            if (row >= 0 && row < (int)s_wifi_ssid_count &&
                ((int)y - 78) % 48 <= 40) {
                s_wifi_selected = (uint8_t)row;
                s_wifi_editor_mode = 2;
                memset(s_wifi_password, 0, sizeof(s_wifi_password));
                s_wifi_uppercase = false;
                s_wifi_symbols = false;
                s_wifi_reveal = true;
                settings_click_feedback();
                refresh_wifi_editor();
            }
        }
        return;
    }
    if (y >= 352 && y <= 402) {
        if (x < LCD_H_RES / 2) {
            settings_click_feedback();
            if (s_wifi_editor_mode == 3) {
                close_wifi_editor();
                return;
            }
            s_wifi_editor_mode = 1;
            memset(s_wifi_password, 0, sizeof(s_wifi_password));
            refresh_wifi_editor();
        } else if (s_wifi_editor_mode == 3) {
            settings_click_feedback();
            if (s_wifi_password[0] == '\0') {
                render_show_message(ui_text("请输入城市", "Enter a city"));
                return;
            }
            const esp_err_t err = weather_use_manual_city(s_wifi_password);
            close_wifi_editor();
            render_show_message(err == ESP_OK ?
                                ui_text("城市更新中", "Updating city") :
                                ui_text("城市设置失败", "City setup failed"));
        } else if (s_wifi_selected < s_wifi_ssid_count) {
            settings_click_feedback();
            const esp_err_t err = wifi_setup_connect(
                s_wifi_ssids[s_wifi_selected], s_wifi_password, 480);
            if (err == ESP_OK && !s_settings.wifi_enabled) {
                s_settings.wifi_enabled = true;
                save_settings();
            }
            close_wifi_editor();
            render_set_wifi_notice(err == ESP_OK ? 1 : 3);
        }
        return;
    }
    if (y >= 299 && y <= 340) {
        if (x >= 45 && x <= 122) s_wifi_symbols = !s_wifi_symbols;
        else if (x >= 128 && x <= 205 && !s_wifi_symbols) {
            s_wifi_uppercase = !s_wifi_uppercase;
        } else if (x >= 211 && x <= 298) {
            const size_t length = strlen(s_wifi_password);
            if (length < sizeof(s_wifi_password) - 1) {
                s_wifi_password[length] = ' ';
                s_wifi_password[length + 1] = '\0';
            }
        }
        else if (x >= 304 && x <= 421) {
            const size_t length = strlen(s_wifi_password);
            if (length > 0) s_wifi_password[length - 1] = '\0';
        } else return;
        settings_click_feedback();
        refresh_wifi_editor();
        return;
    }
    const char key = wifi_key_at(x, y);
    const size_t length = strlen(s_wifi_password);
    if (key != '\0' && length < sizeof(s_wifi_password) - 1) {
        s_wifi_password[length] = key;
        s_wifi_password[length + 1] = '\0';
        settings_click_feedback();
        refresh_wifi_editor();
    }
}

static void handle_operation_guide_tap(uint16_t x, uint16_t y)
{
    if (y >= 370 && y <= 440) {
        if (x <= 228) {
            settings_click_feedback();
            set_settings_open(false);
            ESP_LOGI(TAG, "operation guide: returned to particle scene");
        } else if (x >= 238) {
            settings_click_feedback();
            set_operation_guide(false, 0);
            ESP_LOGI(TAG, "operation guide: returned to settings");
        }
        return;
    }
    if (y < 54 || y > 104) return;
    uint8_t page = s_operation_guide_page;
    if (x >= 82 && x <= 226) page = 0;
    else if (x >= 240 && x <= 384) page = 1;
    else return;
    settings_click_feedback();
    if (page != s_operation_guide_page) {
        set_operation_guide(true, page);
    }
}

static uint8_t panel_brightness(uint8_t percent)
{
    // Keep the minimum visible so a user can always recover the slider.
    return (uint8_t)(12 + (uint16_t)percent * 243 / 100);
}

static void poll_settings_input(void)
{
    bool pressed = false;
    uint16_t x = 0, y = 0;
    if (touch_read_raw(&pressed, &x, &y) != ESP_OK) return;
    (void)x;
    if (pressed) {
        if (!s_settings_touch_down) {
            s_settings_touch_down = true;
            s_settings_touch_x = x;
            s_settings_touch_y = y;
            s_settings_touch_last_x = x;
            s_settings_touch_last_y = y;
            if (s_operation_guide_open) s_settings_touch_target = 2;
            else if (y >= 78 && y <= 443) s_settings_touch_target = 1;
            else s_settings_touch_target = 0;
        } else {
            s_settings_touch_last_x = x;
            s_settings_touch_last_y = y;
        }
    } else if (s_settings_touch_down) {
        const uint8_t target = s_settings_touch_target;
        s_settings_touch_down = false;
        s_settings_touch_target = 0;
        if (target == 1) {
            if (s_wifi_editor_mode != 0) {
                handle_wifi_editor_tap(s_settings_touch_last_x,
                                       s_settings_touch_last_y);
            } else {
                const int dx = (int)s_settings_touch_last_x -
                               (int)s_settings_touch_x;
                const int dy = (int)s_settings_touch_last_y -
                               (int)s_settings_touch_y;
                if (abs(dx) >= 56 && abs(dx) > abs(dy)) {
                    s_settings_page = dx < 0 ? 1 : 0;
                    settings_click_feedback();
                    refresh_settings_screen();
                } else {
                    handle_settings_tap(s_settings_touch_x, s_settings_touch_y);
                }
            }
        } else if (target == 2) {
            handle_operation_guide_tap(s_settings_touch_x,
                                       s_settings_touch_y);
        }
    }
}

static void handle_button(button_event_t event)
{
    if (s_settings_open) {
        if (!s_operation_guide_open && s_wifi_editor_mode == 0) {
            if (event == BUTTON_EVENT_A_SHORT) {
                s_settings_page = 0;
                settings_click_feedback();
                refresh_settings_screen();
            } else if (event == BUTTON_EVENT_B_SHORT) {
                s_settings_page = 1;
                settings_click_feedback();
                refresh_settings_screen();
            }
        }
        if (event == BUTTON_EVENT_AB_LONG) {
            if (s_operation_guide_open) set_operation_guide(false, 0);
            else set_settings_open(false);
        }
        return;
    }
    if (s_weather_open && event != BUTTON_EVENT_NONE) {
        if (event == BUTTON_EVENT_AB_LONG) {
            close_weather_screen();
            set_settings_open(true);
        } else if (event == BUTTON_EVENT_A_SHORT || event == BUTTON_EVENT_B_SHORT) {
            close_weather_screen();
        }
        return;
    }
    if (handwriting_active() && event != BUTTON_EVENT_A_SHORT && event != BUTTON_EVENT_NONE) {
        return;
    }
    switch (event) {
        case BUTTON_EVENT_A_SHORT:
            set_countdown_menu(false);
            if (handwriting_active()) {
                handwriting_cancel();
                touch_cancel_gestures();
                start_handwriting_playback(true);
                render_show_message(ui_text("已取消手写", "Drawing canceled"));
                ESP_LOGI(TAG, "button A: cancel handwriting");
            } else {
                s_clock_held = false;
                s_handwriting_playing = false;
                s_handwriting_restore_ready = false;
                s_countdown_active = false;
                s_countdown_paused = false;
                render_set_countdown_runtime(false, 0, 0.0f, false);
                sim_end_formation();
                handwriting_enter();
                touch_cancel_gestures();
                if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                ESP_LOGI(TAG, "button A: enter handwriting editor");
            }
            break;
        case BUTTON_EVENT_A_DOUBLE:
            if (s_countdown_active) cancel_countdown();
            if (s_countdown_menu) set_countdown_menu(false);
            else set_countdown_menu(true);
            if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
            ESP_LOGI(TAG, "button A double: countdown menu %s",
                     s_countdown_menu ? "open" : "closed");
            break;
        case BUTTON_EVENT_A_LONG:
            s_settings.density = (s_settings.density + 1) % 3;
            ESP_LOGI(TAG, "button A long: density %u (%d particles)",
                     s_settings.density, sim_set_density_mode(s_settings.density));
            if (s_clock_held) {
                if (s_clock_analog) {
                    sim_hold_analog_time(s_clock_hours, s_clock_minutes,
                                         s_clock_seconds);
                } else sim_hold_time(s_clock_hours, s_clock_minutes);
            }
            if (s_audio_ready) audio_trigger(AUDIO_EVENT_DENSITY);
            save_settings();
            render_show_message(s_settings.language ? s_density_labels_en[s_settings.density] :
                                                       s_density_labels[s_settings.density]);
            break;
        case BUTTON_EVENT_B_SHORT:
            set_theme(1);
            break;
        case BUTTON_EVENT_B_DOUBLE: {
            if (s_countdown_active || s_countdown_menu) break;
            uint8_t percent = 0;
            uint16_t millivolts = 0;
            const esp_err_t err = s_battery_ready ?
                battery_read(&percent, &millivolts) : ESP_ERR_INVALID_STATE;
            if (err == ESP_OK) {
                s_clock_held = false;
                s_handwriting_playing = false;
                s_handwriting_restore_ready = false;
                sim_show_battery(percent);
                if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                ESP_LOGI(TAG, "button B double: battery %u%% (%u mV)",
                         (unsigned)percent, (unsigned)millivolts);
            } else {
                render_show_message(ui_text("电量未就绪", "Battery unavailable"));
                ESP_LOGW(TAG, "battery display unavailable: %s",
                         esp_err_to_name(err));
            }
            break;
        }
        case BUTTON_EVENT_B_LONG:
            toggle_reactive();
            break;
        case BUTTON_EVENT_AB_LONG:
            set_settings_open(true);
            ESP_LOGI(TAG, "A+B long: settings opened");
            break;
        default:
            break;
    }
}

static void handle_touch(touch_event_t event, uint16_t x, uint16_t y)
{
    if (s_settings_open) {
        if (event == TOUCH_EVENT_TAP) handle_settings_tap(x, y);
        return;
    }
    if (s_countdown_menu) {
        // The selector consumes raw touch samples in sim_task so dragging is
        // continuous instead of becoming a swipe only after release.
        return;
    }
    if (s_weather_open) {
        if (event == TOUCH_EVENT_SWIPE_RIGHT) {
            close_weather_screen();
            ESP_LOGI(TAG, "weather page closed");
        } else if (event == TOUCH_EVENT_TAP) {
            close_weather_screen();
            ESP_LOGI(TAG, "weather particles released to fluid");
        }
        return;
    }
    if (s_handwriting_playing) {
        if (event == TOUCH_EVENT_DOUBLE_TAP) {
            s_handwriting_playing = false;
            s_handwriting_restore_ready = true;
            sim_release_formation();
            ESP_LOGI(TAG, "touch double tap: handwriting dissolved to fluid");
        } else if (event == TOUCH_EVENT_SWIPE_UP) adjust_main_volume(1);
        else if (event == TOUCH_EVENT_SWIPE_DOWN) adjust_main_volume(-1);
        return;
    }
    if (s_countdown_active) {
        if (event == TOUCH_EVENT_DOUBLE_TAP) {
            cancel_countdown();
            return;
        }
        if (event == TOUCH_EVENT_TAP) {
            const int dx = (int)x - COUNTDOWN_CENTER_X;
            const int dy = (int)y - 334;
            if (dx * dx + dy * dy <= 52 * 52) toggle_countdown_pause();
            return;
        }
        if (event == TOUCH_EVENT_SWIPE_UP) { adjust_main_volume(1); return; }
        if (event == TOUCH_EVENT_SWIPE_DOWN) { adjust_main_volume(-1); return; }
        if (event != TOUCH_EVENT_LONG_PRESS) return;
        cancel_countdown();
        return;
    }
    switch (event) {
        case TOUCH_EVENT_TAP: {
            s_random_shape_transition_pending = false;
            uint8_t hours = 0, minutes = 0, seconds = 0;
            const esp_err_t err = rtc_clock_read_hms(&hours, &minutes, &seconds);
            if (err == ESP_OK) {
                s_clock_hours = hours;
                s_clock_minutes = minutes;
                s_clock_seconds = seconds;
                if (s_clock_held) {
                    if (s_clock_analog) sim_hold_analog_time(hours, minutes, seconds);
                    else sim_hold_time(hours, minutes);
                } else {
                    if (s_clock_analog) sim_show_analog_time(hours, minutes, seconds);
                    else sim_show_time(hours, minutes);
                }
                if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                ESP_LOGI(TAG, "touch particle clock: %02u:%02u at %u,%u (%s)",
                         hours, minutes, x, y, s_clock_held ? "held" : "timed");
            } else {
                render_show_message(ui_text("时钟未就绪", "Clock unavailable"));
                ESP_LOGW(TAG, "particle clock unavailable: %s", esp_err_to_name(err));
            }
            break;
        }
        case TOUCH_EVENT_LONG_PRESS:
            s_random_shape_transition_pending = false;
            if (s_clock_held) {
                s_clock_held = false;
                s_clock_analog = false;
                s_clock_style_transition_pending = false;
                sim_release_formation();
                if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                ESP_LOGI(TAG, "touch long press: held clock released to fluid");
            } else {
                uint8_t hours = 0, minutes = 0, seconds = 0;
                const esp_err_t err = rtc_clock_read_hms(&hours, &minutes, &seconds);
                if (err == ESP_OK) {
                    s_clock_held = true;
                    s_clock_analog = false;
                    s_clock_hours = hours;
                    s_clock_minutes = minutes;
                    s_clock_seconds = seconds;
                    s_clock_last_poll_us = esp_timer_get_time();
                    sim_hold_time(hours, minutes);
                    if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                    render_show_message(ui_text("时钟开启", "Clock locked"));
                    ESP_LOGI(TAG, "touch long press: held clock on at %02u:%02u",
                             hours, minutes);
                } else {
                    render_show_message(ui_text("时钟未就绪", "Clock unavailable"));
                    ESP_LOGW(TAG, "held clock unavailable: %s",
                             esp_err_to_name(err));
                }
            }
            break;
        case TOUCH_EVENT_DOUBLE_TAP:
            if (sim_clock_active()) {
                uint8_t hours = 0, minutes = 0, seconds = 0;
                if (rtc_clock_read_hms(&hours, &minutes, &seconds) == ESP_OK) {
                    s_clock_hours = hours;
                    s_clock_minutes = minutes;
                    s_clock_seconds = seconds;
                    s_clock_style_transition_target_analog = !s_clock_analog;
                    s_clock_analog = s_clock_style_transition_target_analog;
                    sim_release_formation();
                    s_clock_style_transition_pending = true;
                    s_clock_style_transition_deadline_us =
                        esp_timer_get_time() + 650000;
                    if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                    ESP_LOGI(TAG, "clock style transition queued: %s",
                             s_clock_analog ? "analogue" : "digital");
                }
            } else if (s_handwriting_restore_ready && handwriting_count() > 0) {
                start_handwriting_playback(false);
                ESP_LOGI(TAG, "touch double tap: handwriting restored from fluid");
            } else {
                if (sim_random_shape_active() || sim_formation_active()) {
                    sim_release_formation();
                    s_random_shape_transition_pending = true;
                    s_random_shape_transition_deadline_us =
                        esp_timer_get_time() + 650000;
                    ESP_LOGI(TAG, "random shape transition: old formation released");
                } else {
                    sim_show_random_shape();
                }
                if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
            }
            break;
        case TOUCH_EVENT_SWIPE_LEFT:
            s_clock_style_transition_pending = false;
            s_random_shape_transition_pending = false;
            s_weather_open = true;
            if (sim_formation_active()) {
                // Formation-to-formation transitions pass through real fluid:
                // release the old clock, let it visibly fall, then form weather.
                sim_release_formation();
                s_weather_transition_pending = true;
                s_weather_transition_deadline_us =
                    esp_timer_get_time() + 650000;
                refresh_weather_screen();
                ESP_LOGI(TAG, "weather transition: old formation released");
            } else {
                s_weather_transition_pending = false;
                refresh_weather_screen();
            }
            // Enter instantly from the NVS-backed snapshot. Network refreshes
            // stay periodic, or explicit on tap, so opening the particle view
            // never pauses display DMA while HTTP is active.
            ESP_LOGI(TAG, "weather page opened from cached snapshot");
            break;
        case TOUCH_EVENT_SWIPE_RIGHT:
            sim_directional_gust(1.0f, 0.0f);
            render_show_message(ui_text("粒子风：右", "Particle wind: Right"));
            break;
        case TOUCH_EVENT_SWIPE_UP:
            adjust_main_volume(1);
            break;
        case TOUCH_EVENT_SWIPE_DOWN:
            adjust_main_volume(-1);
            break;
        default:
            break;
    }
}

static esp_err_t i2c_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {.clk_speed = 400000},
        .clk_flags = 0,
    };
    s_i2c_bus = i2c_bus_create(I2C_PORT, &config);
    return s_i2c_bus != NULL ? ESP_OK : ESP_FAIL;
}

static bool apply_ble_time(void)
{
    int64_t unix_seconds = 0;
    int16_t timezone_minutes = 0;
    if (!ble_setup_take_time(&unix_seconds, &timezone_minutes)) return false;
    const esp_err_t err = rtc_clock_set_unix(unix_seconds, timezone_minutes);
    if (err == ESP_OK) {
        s_ble_time_calibrating = false;
        s_ble_time_calibrated = true;
        if (s_settings_open) refresh_settings_screen();
        render_show_message(ui_text("已校准", "Synced"));
        ESP_LOGI(TAG, "phone time calibration completed");
        if (s_clock_held) {
            uint8_t hours = 0, minutes = 0, seconds = 0;
            if (rtc_clock_read_hms(&hours, &minutes, &seconds) == ESP_OK) {
                s_clock_hours = hours;
                s_clock_minutes = minutes;
                s_clock_seconds = seconds;
                if (s_clock_analog) sim_hold_analog_time(hours, minutes, seconds);
                else sim_hold_time(hours, minutes);
            }
        }
        return true;
    }
    s_ble_time_calibrating = false;
    if (s_settings_open) refresh_settings_screen();
    ESP_LOGW(TAG, "phone time rejected: %s", esp_err_to_name(err));
    return false;
}

static bool apply_wifi_time(void)
{
    int64_t unix_seconds = 0;
    int16_t timezone_minutes = 0;
    if (!wifi_setup_take_time(&unix_seconds, &timezone_minutes)) return false;
    const esp_err_t err = rtc_clock_set_unix(unix_seconds, timezone_minutes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi SNTP time rejected: %s", esp_err_to_name(err));
        return false;
    }
    render_show_message(ui_text("Wi-Fi 已自动校时", "Wi-Fi time synced"));
    ESP_LOGI(TAG, "background Wi-Fi time calibration completed");
    if (s_clock_held) {
        uint8_t hours = 0, minutes = 0, seconds = 0;
        if (rtc_clock_read_hms(&hours, &minutes, &seconds) == ESP_OK) {
            s_clock_hours = hours;
            s_clock_minutes = minutes;
            s_clock_seconds = seconds;
            if (s_clock_analog) sim_hold_analog_time(hours, minutes, seconds);
            else sim_hold_time(hours, minutes);
        }
    }
    return true;
}

static void render_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (render_frame()) s_frames++;
        else vTaskDelay(pdMS_TO_TICKS(4));
        vTaskDelay(1);
    }
}

static void sim_task(void *arg)
{
    (void)arg;
    sim_forces_t forces = {
        .gravity = {0.0f, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0.0f},
        .down = {0.0f, 1.0f, 0.0f},
        .omega = {0.0f, 0.0f, 0.0f},
        .alpha = {0.0f, 0.0f, 0.0f},
    };

    int64_t last_us = esp_timer_get_time();
    int64_t last_button_us = last_us;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        float dt = (float)(now - last_us) * 1e-6f;
        last_us = now;
        if (dt > 0.05f) {
            dt = 0.05f;
        } else if (dt < 1e-4f) {
            dt = 1e-4f;
        }

        imu_read(dt, &forces);
        sim_step(dt, &forces);
        s_steps++;

        if (now - last_button_us >= BUTTON_PERIOD_MS * 1000) {
            last_button_us = now;
            handle_button(button_poll());
            const ble_setup_state_t ble_state = ble_setup_state();
            if (ble_state != s_settings_ble_state_seen) {
                if (ble_state != BLE_SETUP_CONNECTED) {
                    s_ble_time_calibrating = false;
                    s_ble_time_calibrated = false;
                }
                s_settings_ble_state_seen = ble_state;
                if (s_settings_open) refresh_settings_screen();
            }
            const wifi_setup_state_t wifi_state = wifi_setup_state();
            if (wifi_state != s_settings_wifi_state_seen) {
                const wifi_setup_state_t previous_wifi_state =
                    s_settings_wifi_state_seen;
                s_settings_wifi_state_seen = wifi_state;
                if (s_settings_open) {
                    refresh_settings_screen();
                    if (wifi_state == WIFI_SETUP_CONNECTED) {
                        render_set_wifi_notice(2);
                    } else if (wifi_state == WIFI_SETUP_ERROR &&
                               previous_wifi_state == WIFI_SETUP_CONNECTING) {
                        render_set_wifi_notice(3);
                    }
                }
                ESP_LOGI(TAG, "Wi-Fi state changed: %d", (int)wifi_state);
                if (wifi_state == WIFI_SETUP_CONNECTED) {
                    weather_snapshot_t cached_weather;
                    weather_snapshot(&cached_weather);
                    if (cached_weather.valid && !cached_weather.daily_valid) {
                        weather_refresh();
                        ESP_LOGI(TAG, "one-time daily weather cache migration started");
                    }
                }
            }
            weather_snapshot_t weather;
            weather_snapshot(&weather);
            if (s_weather_transition_pending &&
                now >= s_weather_transition_deadline_us) {
                s_weather_transition_pending = false;
                if (s_weather_open) {
                    refresh_weather_screen();
                    ESP_LOGI(TAG, "weather transition: new formation started");
                }
            }
            if (s_clock_style_transition_pending &&
                now >= s_clock_style_transition_deadline_us) {
                s_clock_style_transition_pending = false;
                if (!s_weather_open && !s_settings_open && !s_countdown_active) {
                    uint8_t hours = 0, minutes = 0, seconds = 0;
                    if (rtc_clock_read_hms(&hours, &minutes, &seconds) == ESP_OK) {
                        s_clock_hours = hours;
                        s_clock_minutes = minutes;
                        s_clock_seconds = seconds;
                        if (s_clock_style_transition_target_analog) {
                            if (s_clock_held) sim_hold_analog_time(hours, minutes, seconds);
                            else sim_show_analog_time(hours, minutes, seconds);
                        } else {
                            if (s_clock_held) sim_hold_time(hours, minutes);
                            else sim_show_time(hours, minutes);
                        }
                        ESP_LOGI(TAG, "clock style transition started: %s",
                                 s_clock_style_transition_target_analog ?
                                 "analogue" : "digital");
                    }
                }
            }
            if (s_random_shape_transition_pending &&
                now >= s_random_shape_transition_deadline_us) {
                s_random_shape_transition_pending = false;
                if (!s_weather_open && !s_settings_open &&
                    !s_countdown_active && !s_handwriting_playing) {
                    sim_show_random_shape();
                    ESP_LOGI(TAG, "random shape transition: new shape started");
                }
            }
            if (weather.state != s_weather_state_seen) {
                s_weather_state_seen = weather.state;
                render_set_network_busy(weather.state == WEATHER_STATE_UPDATING);
                if (s_weather_open) refresh_weather_screen();
                if (s_settings_open) refresh_settings_screen();
            }
            uint16_t x = 0, y = 0;
            if (s_settings_open) {
                poll_settings_input();
            } else if (handwriting_active()) {
                bool pressed = false;
                if (touch_read_raw(&pressed, &x, &y) == ESP_OK) {
                    const handwriting_result_t result = handwriting_poll(pressed, x, y);
                    if (result == HANDWRITING_RESULT_SAVED) {
                        touch_cancel_gestures();
                        if (s_audio_ready) audio_trigger(AUDIO_EVENT_TOUCH);
                        start_handwriting_playback(true);
                        ESP_LOGI(TAG, "handwriting editor finished");
                    }
                }
            } else if (s_countdown_menu) {
                // A rotary selector needs every live CST820 coordinate sample,
                // not a TAP/SWIPE event emitted only after the finger lifts.
                // Keep this path parallel to the handwriting editor, which is
                // already proven to receive continuous touch coordinates.
                poll_countdown_selector();
            } else {
                const touch_event_t touch_event = touch_poll(&x, &y);
                handle_touch(touch_event, x, y);
            }
            if (s_settings.bluetooth_enabled) {
                apply_ble_time();
            }
            apply_wifi_time();
            if (s_ble_time_calibrating &&
                now >= s_ble_calibration_deadline_us) {
                s_ble_time_calibrating = false;
                if (s_settings_open) refresh_settings_screen();
                render_show_message(ui_text("校准超时", "Calibration timed out"));
                ESP_LOGW(TAG, "phone time calibration timed out");
            }
            if (s_handwriting_playing && now >= s_handwriting_next_us &&
                handwriting_count() > 1) {
                s_handwriting_index = (s_handwriting_index + 1) % handwriting_count();
                sim_show_handwriting(handwriting_glyph(s_handwriting_index),
                                     HANDWRITING_W, HANDWRITING_H,
                                     handwriting_glyph_color(s_handwriting_index));
                s_handwriting_next_us = now + HANDWRITING_GLYPH_US;
            }
            if (s_countdown_menu && now >= s_countdown_menu_until_us) {
                set_countdown_menu(false);
            }
            if (s_countdown_active) {
                // A countdown can start while handling input above, after the
                // loop's `now` snapshot was taken. Read a fresh timestamp so
                // 03:00 never rounds up and flashes as 03:01 for one frame.
                const int64_t timer_now = esp_timer_get_time();
                const int64_t remaining_us = s_countdown_paused ?
                    s_countdown_pause_remaining_us : s_countdown_end_us - timer_now;
                if (remaining_us <= 0 && !s_countdown_paused) {
                    s_countdown_active = false;
                    s_countdown_paused = false;
                    s_countdown_last_seconds = -1;
                    render_set_countdown_runtime(false, 0, 1.0f, false);
                    sim_release_formation();
                    if (s_audio_ready) audio_trigger(AUDIO_EVENT_COUNTDOWN);
                    ESP_LOGI(TAG, "particle countdown complete");
                } else {
                    const int remaining_seconds = (int)((remaining_us + 999999) / 1000000);
                    const float ratio = (float)remaining_us /
                                        (float)s_countdown_total_us;
                    if (remaining_seconds != s_countdown_last_seconds) {
                        if (s_audio_ready && s_countdown_last_seconds >= 0 &&
                            remaining_seconds < s_countdown_last_seconds) {
                            audio_trigger_countdown_tick(remaining_seconds <= 5 ? 1 : 0);
                        }
                        s_countdown_last_seconds = remaining_seconds;
                        if (remaining_seconds >= 3600) {
                            const int rounded_minutes = (remaining_seconds + 59) / 60;
                            sim_show_countdown((uint8_t)(rounded_minutes / 60),
                                               (uint8_t)(rounded_minutes % 60), ratio);
                        } else {
                            sim_show_countdown((uint8_t)(remaining_seconds / 60),
                                               (uint8_t)(remaining_seconds % 60), ratio);
                        }
                    }
                }
            }
            if (s_clock_held && !s_weather_open &&
                now - s_clock_last_poll_us >= 1000000) {
                s_clock_last_poll_us = now;
                uint8_t hours = 0, minutes = 0, seconds = 0;
                if (rtc_clock_read_hms(&hours, &minutes, &seconds) == ESP_OK &&
                    (hours != s_clock_hours || minutes != s_clock_minutes)) {
                    s_clock_hours = hours;
                    s_clock_minutes = minutes;
                    s_clock_seconds = seconds;
                    if (s_clock_analog) sim_hold_analog_time(hours, minutes, seconds);
                    else sim_hold_time(hours, minutes);
                    ESP_LOGI(TAG, "held particle clock advanced to %02u:%02u",
                             hours, minutes);
                }
            }
            sim_stats_t stats;
            sim_stats(&stats);
            if (s_audio_ready) {
                audio_set_motion(stats.mean_speed, stats.max_speed,
                                 stats.front_hits + stats.back_hits,
                                 stats.clamped);
                if (s_settings.reactive) {
                    float bass = 0.0f;
                    audio_get_levels(&bass, NULL, NULL);
                    if (bass > 0.68f && s_last_bass <= 0.68f) {
                        sim_audio_pulse(bass);
                    }
                    s_last_bass = bass;
                }
            }
        }
        vTaskDelay(SIM_YIELD_TICKS);
    }
}

static void stats_loop(void)
{
    uint32_t last_frames = 0;
    uint32_t last_steps = 0;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
        const int64_t now = esp_timer_get_time();
        const float elapsed = (float)(now - last_us) * 1e-6f;
        last_us = now;

        const uint32_t frames = s_frames;
        const uint32_t steps = s_steps;
        sim_stats_t stats;
        sim_stats(&stats);
        float accel[3];
        imu_raw_accel(accel);

        ESP_LOGI(TAG,
                 "%.1f fps | %.1f steps/s | grid %d dens %d relax %d us | "
                 "rho %.2f/%.2f | speed avg %.0f max %.0f | "
                 "accel % .2f % .2f % .2f | sram %u",
                 (double)((frames - last_frames) / elapsed),
                 (double)((steps - last_steps) / elapsed),
                 stats.us_grid, stats.us_density, stats.us_relax,
                 (double)stats.mean_density, (double)stats.rest_density,
                 (double)stats.mean_speed, (double)stats.max_speed,
                 (double)accel[0], (double)accel[1], (double)accel[2],
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        last_frames = frames;
        last_steps = steps;
    }
}

static uint8_t first_run_choose_language(void)
{
    render_show_language_selection();
    touch_cancel_gestures();
    const int64_t deadline = esp_timer_get_time() + 60000000;
    while (s_touch_ready && esp_timer_get_time() < deadline) {
        uint16_t x = 0, y = 0;
        const touch_event_t event = touch_poll(&x, &y);
        if (event == TOUCH_EVENT_TAP) {
            if (y >= 140 && y <= 238) return 0;
            if (y >= 238 && y <= 338) return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "language selection timed out; using Chinese");
    return 0;
}

static bool first_run_configure_bluetooth(uint8_t language)
{
    const esp_err_t start_err = ble_setup_start();
    bool connected = false;
    bool error = start_err != ESP_OK;
    ble_setup_state_t previous = BLE_SETUP_OFF;
    render_show_bluetooth_setup(language, false, error);
    touch_cancel_gestures();
    const int64_t deadline = esp_timer_get_time() + 60000000;

    while (s_touch_ready && esp_timer_get_time() < deadline) {
        const ble_setup_state_t state = ble_setup_state();
        const bool now_connected = state == BLE_SETUP_CONNECTED;
        const bool now_error = state == BLE_SETUP_ERROR;
        if (state != previous || now_connected != connected || now_error != error) {
            previous = state;
            connected = now_connected;
            error = now_error;
            render_show_bluetooth_setup(language, connected, error);
        }
        if (connected) {
            // First-use setup keeps its one-step experience: after pairing,
            // explicitly request one standard Current Time Service read.
            ble_setup_request_time();
            const int64_t sync_deadline = esp_timer_get_time() + 2500000;
            while (esp_timer_get_time() < sync_deadline) {
                int64_t unix_seconds = 0;
                int16_t timezone_minutes = 0;
                if (ble_setup_take_time(&unix_seconds, &timezone_minutes)) {
                    const esp_err_t err = rtc_clock_set_unix(unix_seconds,
                                                             timezone_minutes);
                    ESP_LOGI(TAG, "first-use phone time sync: %s",
                             esp_err_to_name(err));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            return true;
        }

        uint16_t x = 0, y = 0;
        const touch_event_t event = touch_poll(&x, &y);
        if (event == TOUCH_EVENT_TAP && y >= 350) {
            if (x < LCD_H_RES / 2) {
                ble_setup_stop();
                return false;
            }
            return !error;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ble_setup_stop();
    ESP_LOGW(TAG, "Bluetooth setup timed out; continuing with BLE off");
    return false;
}

static void run_first_use_onboarding(void)
{
    render_show_onboarding_faces();
    s_settings.language = first_run_choose_language();
    render_set_language(s_settings.language);
    handwriting_set_language(s_settings.language);
    render_show_boot_splash(s_settings.language);
    s_settings.bluetooth_enabled =
        first_run_configure_bluetooth(s_settings.language);
    s_settings.onboarding_complete = true;
    settings_save(&s_settings);
    ESP_LOGI(TAG, "first-use onboarding complete: language=%s BLE=%s",
             s_settings.language == 0 ? "zh-CN" : "en",
             s_settings.bluetooth_enabled ? "on" : "off");
}

void app_main(void)
{
    ESP_LOGI(TAG, "时迹 ChronoTrace starting");
    settings_init(&s_settings);
    ESP_ERROR_CHECK(i2c_init());
    ESP_ERROR_CHECK(board_power_init(s_i2c_bus));
    if (battery_init(s_i2c_bus) == ESP_OK) {
        s_battery_ready = true;
    } else {
        ESP_LOGW(TAG, "continuing without battery level input");
    }
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(display_set_brightness(panel_brightness(s_settings.brightness)));
    ESP_ERROR_CHECK(button_init());
    if (touch_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without touch input");
    } else {
        s_touch_ready = true;
    }

    if (imu_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without motion input");
    }
    if (rtc_clock_init(s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without hardware clock input");
    }
    sim_init();
    handwriting_init();
    handwriting_set_language(s_settings.language);
    sim_set_density_mode(s_settings.density);
    render_set_theme(s_settings.theme);
    render_set_reactive(s_settings.reactive);
    render_init();
    render_set_language(s_settings.language);
    if (!s_settings.onboarding_complete) {
        run_first_use_onboarding();
    } else {
        render_show_boot_splash(s_settings.language);
    }
    // Reserve NimBLE's contiguous internal-memory allocation before Wi-Fi.
    // When Bluetooth is off, suspend radio activity but retain that allocation
    // so the settings toggle can turn it back on reliably at runtime.
    if (ble_setup_start() != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth host reservation failed");
    } else if (!s_settings.bluetooth_enabled) {
        ble_setup_set_enabled(false);
    }
    // BLE controller needs a sizeable contiguous internal-memory allocation.
    // Reserve it before the Wi-Fi driver claims its RX/TX buffers; otherwise a
    // saved Bluetooth connection can fail at boot and trip the controller
    // assertion watchdog. Wi-Fi/LwIP buffers are configured to prefer PSRAM.
    if (wifi_setup_init(s_settings.wifi_enabled) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without Wi-Fi setup");
    } else {
        s_settings_wifi_state_seen = wifi_setup_state();
    }
    if (weather_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without weather service");
    } else {
        weather_snapshot_t weather;
        weather_snapshot(&weather);
        s_weather_state_seen = weather.state;
        // Do not refresh merely because the device has just booted or joined
        // Wi-Fi. The cached snapshot makes weather entry instant; periodic and
        // tap-requested refreshes remain available without surprising freezes.
    }
    render_show_message(ui_text("操作已就绪", "Ready"));
    if (audio_init(s_i2c_bus, s_settings.theme, s_settings.volume,
                   s_settings.reactive) == ESP_OK) {
        s_audio_ready = true;
    } else {
        ESP_LOGW(TAG, "continuing without audio");
    }
    // Saved handwriting remains available from the yellow-button editor, but
    // boot and firmware flashing must always open on the normal fluid scene.
    // Do not automatically replay persisted glyphs here.
    xTaskCreatePinnedToCore(sim_task, "sim", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(render_task, "render", 4096, NULL, 5, NULL, 0);
    stats_loop();
}
