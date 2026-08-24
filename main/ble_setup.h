#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BLE_SETUP_OFF = 0,
    BLE_SETUP_STARTING,
    BLE_SETUP_ADVERTISING,
    BLE_SETUP_CONNECTED,
    BLE_SETUP_ERROR,
} ble_setup_state_t;

// Starts a connectable BLE peripheral named "ChronoTrace". Calling this more
// than once is harmless and leaves an existing connection/advertisement intact.
esp_err_t ble_setup_start(void);
esp_err_t ble_setup_stop(void);
// Enables or suspends advertising/connection while keeping the NimBLE host
// allocation reserved, so a later enable remains reliable beside Wi-Fi.
esp_err_t ble_setup_set_enabled(bool enabled);
// Starts BLE when needed and, while disconnected, force-refreshes the
// connectable advertisement so the phone picker can discover it again.
esp_err_t ble_setup_make_discoverable(void);
ble_setup_state_t ble_setup_state(void);

// Requests one Current Time Service read from the connected phone. Pairing is
// initiated first when the link has not yet been encrypted.
esp_err_t ble_setup_request_time(void);

// Once paired with iOS, ChronoTrace reads Apple's standard Current Time
// Service. The legacy custom write characteristic remains available as a
// harmless fallback, but no companion app is required for iPhone time sync.
bool ble_setup_take_time(int64_t *unix_seconds, int16_t *timezone_minutes);
