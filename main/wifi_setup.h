#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_SETUP_UNCONFIGURED = 0,
    WIFI_SETUP_CONNECTING,
    WIFI_SETUP_CONNECTED,
    WIFI_SETUP_PORTAL,
    WIFI_SETUP_ERROR,
} wifi_setup_state_t;

// Loads saved credentials and starts a background station connection when a
// previous setup exists. It is safe to call after NVS and the hardware RTC are
// ready; no access point is created until the user explicitly asks for it.
esp_err_t wifi_setup_init(bool enabled);
esp_err_t wifi_setup_set_enabled(bool enabled);
bool wifi_setup_enabled(void);

// Opens a temporary WPA2 access point named ChronoTrace-Setup. iPhone, Android,
// and desktop browsers can configure the home network at 192.168.4.1 without a
// companion app. The portal closes automatically after saving or timing out.
esp_err_t wifi_setup_start_portal(void);

wifi_setup_state_t wifi_setup_state(void);
bool wifi_setup_has_credentials(void);

// Device-side configuration helpers. The settings UI scans nearby access
// points, lets the user select one, and supplies the password from its touch
// keyboard. SSIDs are UTF-8 byte strings, each capped at the Wi-Fi limit.
esp_err_t wifi_setup_scan(char (*ssids)[33], size_t capacity, size_t *count);
esp_err_t wifi_setup_connect(const char *ssid, const char *password,
                             int16_t timezone_minutes);

// SNTP callbacks are converted into a single pending UTC timestamp so the main
// task can update the RX8130 hardware clock on its normal control path.
bool wifi_setup_take_time(int64_t *unix_seconds, int16_t *timezone_minutes);
