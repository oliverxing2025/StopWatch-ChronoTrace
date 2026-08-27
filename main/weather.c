#include "weather.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define WEATHER_REFRESH_MS (30 * 60 * 1000)
#define HTTP_CAPACITY 6144

static const char *TAG = "weather";
static esp_timer_handle_t s_periodic_timer;
static bool s_busy;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_snapshot_t s_snapshot = {
    .state = WEATHER_STATE_IDLE,
    .automatic_location = true,
};
static float s_latitude;
static float s_longitude;
static bool s_have_coordinates;
static char s_manual_query[49];

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_buffer_t *buffer = (http_buffer_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buffer || !event->data ||
        event->data_len <= 0) return ESP_OK;
    const size_t available = buffer->capacity - buffer->length - 1;
    const size_t copy = (size_t)event->data_len < available ?
                        (size_t)event->data_len : available;
    if (copy > 0) {
        memcpy(buffer->data + buffer->length, event->data, copy);
        buffer->length += copy;
        buffer->data[buffer->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, char **response)
{
    if (!url || !response) return ESP_ERR_INVALID_ARG;
    char *storage = heap_caps_malloc(HTTP_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) storage = malloc(HTTP_CAPACITY);
    if (!storage) return ESP_ERR_NO_MEM;
    storage[0] = '\0';
    http_buffer_t buffer = {.data = storage, .capacity = HTTP_CAPACITY};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &buffer,
        .timeout_ms = 12000,
        .buffer_size = 512,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(storage);
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || buffer.length == 0 ||
        buffer.length + 1 >= buffer.capacity) {
        ESP_LOGW(TAG, "GET failed: err=%s status=%d bytes=%u", esp_err_to_name(err),
                 status, (unsigned)buffer.length);
        free(storage);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    *response = storage;
    return ESP_OK;
}

static void copy_json_string(char *destination, size_t capacity,
                             const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        strlcpy(destination, value->valuestring, capacity);
    }
}

static void url_encode(char *destination, size_t capacity, const char *source)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; source[i] && out + 1 < capacity; i++) {
        const unsigned char c = (unsigned char)source[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            destination[out++] = (char)c;
        } else if (out + 3 < capacity) {
            destination[out++] = '%';
            destination[out++] = hex[c >> 4];
            destination[out++] = hex[c & 15];
        }
    }
    destination[out] = '\0';
}

static esp_err_t load_cache(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("chronowthr", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t value = 1;
    if (nvs_get_u8(handle, "auto", &value) == ESP_OK) {
        s_snapshot.automatic_location = value != 0;
    }
    size_t length = sizeof(s_manual_query);
    if (nvs_get_str(handle, "query", s_manual_query, &length) != ESP_OK) {
        s_manual_query[0] = '\0';
    }
    length = sizeof(s_snapshot.city);
    if (nvs_get_str(handle, "city", s_snapshot.city, &length) != ESP_OK) {
        s_snapshot.city[0] = '\0';
    }
    if (nvs_get_blob(handle, "lat", &s_latitude,
                     &(size_t){sizeof(s_latitude)}) == ESP_OK &&
        nvs_get_blob(handle, "lon", &s_longitude,
                     &(size_t){sizeof(s_longitude)}) == ESP_OK) {
        s_have_coordinates = true;
    }
    uint8_t valid = 0;
    nvs_get_u8(handle, "valid", &valid);
    if (valid) {
        size_t size = sizeof(s_snapshot.temperature_c);
        nvs_get_blob(handle, "temp", &s_snapshot.temperature_c, &size);
        size = sizeof(s_snapshot.minimum_c);
        const esp_err_t minimum_err =
            nvs_get_blob(handle, "tmin", &s_snapshot.minimum_c, &size);
        if (minimum_err != ESP_OK) {
            s_snapshot.minimum_c = s_snapshot.temperature_c;
        }
        size = sizeof(s_snapshot.maximum_c);
        const esp_err_t maximum_err =
            nvs_get_blob(handle, "tmax", &s_snapshot.maximum_c, &size);
        if (maximum_err != ESP_OK) {
            s_snapshot.maximum_c = s_snapshot.temperature_c;
        }
        uint8_t daily_valid = 0;
        nvs_get_u8(handle, "daily", &daily_valid);
        s_snapshot.daily_valid = daily_valid != 0 &&
                                 minimum_err == ESP_OK && maximum_err == ESP_OK;
        size = sizeof(s_snapshot.apparent_c);
        nvs_get_blob(handle, "feel", &s_snapshot.apparent_c, &size);
        size = sizeof(s_snapshot.wind_kmh);
        nvs_get_blob(handle, "wind", &s_snapshot.wind_kmh, &size);
        nvs_get_u8(handle, "humid", &s_snapshot.humidity);
        nvs_get_u8(handle, "code", &s_snapshot.weather_code);
        nvs_get_i64(handle, "updated", &s_snapshot.updated_unix);
        s_snapshot.valid = true;
        s_snapshot.state = WEATHER_STATE_READY;
    }
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_cache(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("chronowthr", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_u8(handle, "auto", s_snapshot.automatic_location)) == ESP_OK &&
        (err = nvs_set_str(handle, "query", s_manual_query)) == ESP_OK &&
        (err = nvs_set_str(handle, "city", s_snapshot.city)) == ESP_OK &&
        (err = nvs_set_blob(handle, "lat", &s_latitude, sizeof(s_latitude))) == ESP_OK &&
        (err = nvs_set_blob(handle, "lon", &s_longitude, sizeof(s_longitude))) == ESP_OK &&
        (err = nvs_set_u8(handle, "valid", s_snapshot.valid)) == ESP_OK &&
        (err = nvs_set_u8(handle, "daily", s_snapshot.daily_valid)) == ESP_OK &&
        (err = nvs_set_blob(handle, "temp", &s_snapshot.temperature_c,
                            sizeof(s_snapshot.temperature_c))) == ESP_OK &&
        (err = nvs_set_blob(handle, "tmin", &s_snapshot.minimum_c,
                            sizeof(s_snapshot.minimum_c))) == ESP_OK &&
        (err = nvs_set_blob(handle, "tmax", &s_snapshot.maximum_c,
                            sizeof(s_snapshot.maximum_c))) == ESP_OK &&
        (err = nvs_set_blob(handle, "feel", &s_snapshot.apparent_c,
                            sizeof(s_snapshot.apparent_c))) == ESP_OK &&
        (err = nvs_set_blob(handle, "wind", &s_snapshot.wind_kmh,
                            sizeof(s_snapshot.wind_kmh))) == ESP_OK &&
        (err = nvs_set_u8(handle, "humid", s_snapshot.humidity)) == ESP_OK &&
        (err = nvs_set_u8(handle, "code", s_snapshot.weather_code)) == ESP_OK &&
        (err = nvs_set_i64(handle, "updated", s_snapshot.updated_unix)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t locate_automatically(void)
{
    char *response = NULL;
    esp_err_t err = http_get_json(
        "https://ipwho.is/?fields=success,city,latitude,longitude", &response);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    const cJSON *latitude = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    const cJSON *longitude = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    if (!cJSON_IsTrue(success) || !cJSON_IsNumber(latitude) ||
        !cJSON_IsNumber(longitude)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_latitude = (float)latitude->valuedouble;
    s_longitude = (float)longitude->valuedouble;
    copy_json_string(s_snapshot.city, sizeof(s_snapshot.city), root, "city");
    if (s_snapshot.city[0] == '\0') strlcpy(s_snapshot.city, "Current", sizeof(s_snapshot.city));
    s_have_coordinates = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t geocode_manual_city(void)
{
    if (s_manual_query[0] == '\0') return ESP_ERR_INVALID_STATE;
    char encoded[160];
    char url[320];
    url_encode(encoded, sizeof(encoded), s_manual_query);
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
             encoded);
    char *response = NULL;
    esp_err_t err = http_get_json(url, &response);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    const cJSON *latitude = first ? cJSON_GetObjectItemCaseSensitive(first, "latitude") : NULL;
    const cJSON *longitude = first ? cJSON_GetObjectItemCaseSensitive(first, "longitude") : NULL;
    if (!first || !cJSON_IsNumber(latitude) || !cJSON_IsNumber(longitude)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    s_latitude = (float)latitude->valuedouble;
    s_longitude = (float)longitude->valuedouble;
    copy_json_string(s_snapshot.city, sizeof(s_snapshot.city), first, "name");
    if (s_snapshot.city[0] == '\0') strlcpy(s_snapshot.city, s_manual_query,
                                              sizeof(s_snapshot.city));
    s_have_coordinates = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_current_weather(void)
{
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m&daily=temperature_2m_min,temperature_2m_max&timezone=auto&forecast_days=1",
             (double)s_latitude, (double)s_longitude);
    char *response = NULL;
    esp_err_t err = http_get_json(url, &response);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temperature = current ? cJSON_GetObjectItemCaseSensitive(current, "temperature_2m") : NULL;
    const cJSON *apparent = current ? cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature") : NULL;
    const cJSON *humidity = current ? cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m") : NULL;
    const cJSON *code = current ? cJSON_GetObjectItemCaseSensitive(current, "weather_code") : NULL;
    const cJSON *wind = current ? cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m") : NULL;
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    const cJSON *minimums = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min") : NULL;
    const cJSON *maximums = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max") : NULL;
    const cJSON *minimum = cJSON_IsArray(minimums) ? cJSON_GetArrayItem(minimums, 0) : NULL;
    const cJSON *maximum = cJSON_IsArray(maximums) ? cJSON_GetArrayItem(maximums, 0) : NULL;
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(apparent) ||
        !cJSON_IsNumber(humidity) || !cJSON_IsNumber(code) || !cJSON_IsNumber(wind) ||
        !cJSON_IsNumber(minimum) || !cJSON_IsNumber(maximum)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_snapshot.temperature_c = (float)temperature->valuedouble;
    s_snapshot.minimum_c = (float)minimum->valuedouble;
    s_snapshot.maximum_c = (float)maximum->valuedouble;
    s_snapshot.daily_valid = true;
    s_snapshot.apparent_c = (float)apparent->valuedouble;
    s_snapshot.humidity = (uint8_t)humidity->valueint;
    s_snapshot.weather_code = (uint8_t)code->valueint;
    s_snapshot.wind_kmh = (float)wind->valuedouble;
    time(&s_snapshot.updated_unix);
    s_snapshot.valid = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static void update_once(void)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.state = WEATHER_STATE_UPDATING;
    const bool automatic = s_snapshot.automatic_location;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = ESP_OK;
    if (automatic) {
        // Refresh the public-IP position each cycle so the watch follows a
        // genuinely different network instead of permanently caching one city.
        err = locate_automatically();
    } else if (!s_have_coordinates) {
        err = geocode_manual_city();
    }
    if (err == ESP_OK) err = fetch_current_weather();
    portENTER_CRITICAL(&s_lock);
    s_snapshot.state = err == ESP_OK ? WEATHER_STATE_READY : WEATHER_STATE_ERROR;
    portEXIT_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        save_cache();
        // Avoid exposing the user's approximate location in serial logs.
        ESP_LOGI(TAG, "weather updated: %.1f C code=%u",
                 (double)s_snapshot.temperature_c, s_snapshot.weather_code);
    } else {
        ESP_LOGW(TAG, "weather update failed: %s", esp_err_to_name(err));
    }
}

static void weather_task(void *argument)
{
    (void)argument;
    // Give the UI loop time to stop display DMA before Wi-Fi/HTTP allocations.
    vTaskDelay(pdMS_TO_TICKS(75));
    update_once();
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static void periodic_refresh(void *argument)
{
    (void)argument;
    weather_refresh();
}

esp_err_t weather_init(void)
{
    if (s_periodic_timer) return ESP_OK;
    load_cache();
    // Manual city selection has been retired from the watch UI. Migrate any
    // older saved configuration to automatic IP-based location while keeping
    // its cached weather visible until the next explicit/periodic refresh.
    if (!s_snapshot.automatic_location) {
        s_snapshot.automatic_location = true;
        s_manual_query[0] = '\0';
        s_have_coordinates = false;
        s_snapshot.daily_valid = false;
        save_cache();
    }
    const esp_timer_create_args_t timer = {
        .callback = periodic_refresh,
        .name = "weather_periodic",
    };
    esp_err_t err = esp_timer_create(&timer, &s_periodic_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_periodic_timer,
                                       (uint64_t)WEATHER_REFRESH_MS * 1000ULL);
    }
    return err;
}

void weather_refresh(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_busy) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_busy = true;
    s_snapshot.state = WEATHER_STATE_UPDATING;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(weather_task, "weather", 3584, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        s_snapshot.state = WEATHER_STATE_ERROR;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
}

esp_err_t weather_use_automatic_location(void)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.automatic_location = true;
    s_snapshot.daily_valid = false;
    s_have_coordinates = false;
    portEXIT_CRITICAL(&s_lock);
    save_cache();
    weather_refresh();
    return ESP_OK;
}

esp_err_t weather_use_manual_city(const char *city)
{
    if (!city || city[0] == '\0' || strlen(city) >= sizeof(s_manual_query)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_manual_query, city, sizeof(s_manual_query));
    s_snapshot.automatic_location = false;
    s_snapshot.daily_valid = false;
    s_have_coordinates = false;
    portEXIT_CRITICAL(&s_lock);
    save_cache();
    weather_refresh();
    return ESP_OK;
}

void weather_snapshot(weather_snapshot_t *snapshot)
{
    if (!snapshot) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
