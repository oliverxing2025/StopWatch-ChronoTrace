#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static bool s_ready;

esp_err_t settings_init(chrono_trace_settings_t *settings)
{
    if (settings == NULL) return ESP_ERR_INVALID_ARG;

    *settings = (chrono_trace_settings_t){
        .theme = 0,
        .volume = 45,
        .density = 1,
        .reactive = false,
        .language = 0,
        .onboarding_complete = false,
        .bluetooth_enabled = false,
        .wifi_enabled = true,
        .brightness = 62,
        .haptic_enabled = true,
    };

    const esp_err_t init_err = nvs_flash_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s); keeping defaults without erasing",
                 esp_err_to_name(init_err));
        return init_err;
    }
    s_ready = true;

    nvs_handle_t handle;
    // Keep the legacy namespace so an in-place rename preserves the user's
    // selected theme, volume, density, and reactive-mode settings.
    esp_err_t err = nvs_open("fluidbox", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved ChronoTrace settings; using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    uint8_t value;
    if (nvs_get_u8(handle, "theme", &value) == ESP_OK && value < 8) settings->theme = value;
    if (nvs_get_u8(handle, "volume_pct", &value) == ESP_OK && value <= 100) {
        settings->volume = value;
    } else if (nvs_get_u8(handle, "volume", &value) == ESP_OK && value < 3) {
        const uint8_t legacy_volume[3] = {0, 38, 68};
        settings->volume = legacy_volume[value];
    }
    if (nvs_get_u8(handle, "density", &value) == ESP_OK && value < 3) settings->density = value;
    if (nvs_get_u8(handle, "reactive", &value) == ESP_OK) settings->reactive = value != 0;
    if (nvs_get_u8(handle, "language", &value) == ESP_OK && value < 2) settings->language = value;
    if (nvs_get_u8(handle, "onboarded", &value) == ESP_OK) settings->onboarding_complete = value != 0;
    if (nvs_get_u8(handle, "ble_on", &value) == ESP_OK) settings->bluetooth_enabled = value != 0;
    if (nvs_get_u8(handle, "wifi_on", &value) == ESP_OK) settings->wifi_enabled = value != 0;
    if (nvs_get_u8(handle, "bright_pct", &value) == ESP_OK && value <= 100) {
        settings->brightness = value;
    } else if (nvs_get_u8(handle, "bright", &value) == ESP_OK && value < 3) {
        const uint8_t legacy_brightness[3] = {25, 62, 100};
        settings->brightness = legacy_brightness[value];
    }
    if (nvs_get_u8(handle, "haptic", &value) == ESP_OK) {
        settings->haptic_enabled = value != 0;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t settings_save(const chrono_trace_settings_t *settings)
{
    if (settings == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("fluidbox", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_u8(handle, "theme", settings->theme)) == ESP_OK &&
        (err = nvs_set_u8(handle, "volume_pct", settings->volume)) == ESP_OK &&
        (err = nvs_set_u8(handle, "density", settings->density)) == ESP_OK &&
        (err = nvs_set_u8(handle, "reactive", settings->reactive ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(handle, "language", settings->language)) == ESP_OK &&
        (err = nvs_set_u8(handle, "onboarded", settings->onboarding_complete ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(handle, "ble_on", settings->bluetooth_enabled ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(handle, "wifi_on", settings->wifi_enabled ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(handle, "bright_pct", settings->brightness)) == ESP_OK &&
        (err = nvs_set_u8(handle, "haptic", settings->haptic_enabled ? 1 : 0)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t settings_load_handwriting(uint8_t *count, uint8_t *data,
                                    size_t capacity, size_t glyph_bytes,
                                    uint8_t *colors, size_t color_capacity)
{
    if (!count || !data || !colors || glyph_bytes == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("fluidbox", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t saved_count = 0;
    size_t length = capacity;
    err = nvs_get_u8(handle, "hw_count", &saved_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK && saved_count > 0 &&
        (size_t)saved_count * glyph_bytes <= capacity) {
        length = (size_t)saved_count * glyph_bytes;
        err = nvs_get_blob(handle, "hw_glyphs", data, &length);
        if (err == ESP_OK && length == (size_t)saved_count * glyph_bytes) {
            *count = saved_count;
            memset(colors, 0, color_capacity);
            if (color_capacity >= saved_count) {
                size_t color_length = saved_count;
                const esp_err_t color_err = nvs_get_blob(handle, "hw_colors", colors,
                                                          &color_length);
                if (color_err != ESP_OK || color_length != saved_count) {
                    memset(colors, 0, saved_count);
                }
            }
        }
    }
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t settings_save_handwriting(uint8_t count, const uint8_t *data,
                                    size_t glyph_bytes, const uint8_t *colors)
{
    if (!s_ready || glyph_bytes == 0 ||
        (count > 0 && (!data || !colors))) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("fluidbox", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (count > 0) {
        err = nvs_set_blob(handle, "hw_glyphs", data,
                           (size_t)count * glyph_bytes);
        if (err == ESP_OK) err = nvs_set_u8(handle, "hw_count", count);
        if (err == ESP_OK) err = nvs_set_blob(handle, "hw_colors", colors, count);
    } else {
        err = nvs_set_u8(handle, "hw_count", 0);
        if (err == ESP_OK) {
            const esp_err_t glyph_err = nvs_erase_key(handle, "hw_glyphs");
            if (glyph_err != ESP_OK && glyph_err != ESP_ERR_NVS_NOT_FOUND) {
                err = glyph_err;
            }
        }
        if (err == ESP_OK) {
            const esp_err_t color_err = nvs_erase_key(handle, "hw_colors");
            if (color_err != ESP_OK && color_err != ESP_ERR_NVS_NOT_FOUND) {
                err = color_err;
            }
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
