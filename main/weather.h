#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WEATHER_STATE_IDLE = 0,
    WEATHER_STATE_UPDATING,
    WEATHER_STATE_READY,
    WEATHER_STATE_ERROR,
} weather_state_t;

typedef struct {
    weather_state_t state;
    bool valid;
    bool daily_valid;
    bool automatic_location;
    char city[49];
    float temperature_c;
    float minimum_c;
    float maximum_c;
    float apparent_c;
    float wind_kmh;
    uint8_t humidity;
    uint8_t weather_code;
    int64_t updated_unix;
} weather_snapshot_t;

// Loads the cached location and last successful weather sample from NVS, then
// creates the low-priority background updater. Network work starts only after
// weather_refresh() is called for a connected Wi-Fi station.
esp_err_t weather_init(void);
// Starts one asynchronous weather update. Returns ESP_ERR_INVALID_STATE when
// an update is already running, so callers never report a refresh that did not
// actually start.
esp_err_t weather_refresh(void);
esp_err_t weather_use_automatic_location(void);
esp_err_t weather_use_manual_city(const char *city);
void weather_snapshot(weather_snapshot_t *snapshot);
