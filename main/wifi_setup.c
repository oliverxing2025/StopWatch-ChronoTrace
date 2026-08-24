#include "wifi_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#define PORTAL_SSID "ChronoTrace-Setup"
#define PORTAL_PASSWORD "time-trace"
#define PORTAL_TIMEOUT_MS (5 * 60 * 1000)
#define WIFI_RETRY_LIMIT 8

static const char *TAG = "wifi_setup";
static wifi_setup_state_t s_state = WIFI_SETUP_UNCONFIGURED;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_control_task;
static bool s_wifi_ready;
static bool s_enabled = true;
static bool s_has_credentials;
static bool s_portal_running;
static bool s_connect_after_portal;
static bool s_scanning;
static bool s_disconnected_during_scan;
static int s_retry_count;
static int16_t s_timezone_minutes = 480;
static char s_ssid[33];
static char s_password[65];
static char s_city[49];
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_pending;
static int64_t s_pending_unix;

static const char PORTAL_HTML[] =
    "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ChronoTrace Wi-Fi</title><style>"
    "body{margin:0;background:#05080d;color:#f4f7fa;font:16px -apple-system,BlinkMacSystemFont,sans-serif}"
    ".card{max-width:420px;margin:36px auto;padding:28px 24px;background:#101923;border-radius:28px;box-shadow:0 18px 60px #0008}"
    "h1{font-size:28px;margin:0 0 8px}.sub{color:#91a2b2;margin-bottom:24px}"
    "label{display:block;margin:17px 0 7px;color:#c8d2dc}input,select{box-sizing:border-box;width:100%;padding:14px 15px;border:1px solid #294052;border-radius:14px;background:#081018;color:white;font-size:17px}"
    "button{width:100%;margin-top:25px;padding:15px;border:0;border-radius:16px;background:linear-gradient(90deg,#19d8ff,#4ae8aa);color:#031017;font-size:18px;font-weight:700}"
    ".hint{font-size:13px;color:#728493;margin-top:16px}</style></head><body><div class='card'>"
    "<h1>时迹 ChronoTrace</h1><div class='sub'>连接家庭 Wi-Fi，用于天气更新和自动校时</div>"
    "<form method='post' action='/save'><label>Wi-Fi</label><select id='ssid' name='ssid' required><option value=''>正在搜索...</option></select>"
    "<label>密码</label><input name='password' type='text' maxlength='64' autocomplete='off' spellcheck='false'>"
    "<label>城市</label><input name='city' maxlength='48' placeholder='例如：上海 / Shanghai'>"
    "<input id='tz' name='tz' type='hidden'><button type='submit'>保存并连接</button></form>"
    "<div class='hint'>配置仅保存在设备本地。热点将在保存后自动关闭。</div></div>"
    "<script>document.getElementById('tz').value=-new Date().getTimezoneOffset();"
    "fetch('/scan').then(r=>r.json()).then(a=>{let s=document.getElementById('ssid');s.innerHTML='';a.forEach(x=>{let o=document.createElement('option');o.value=x;o.textContent=x;s.appendChild(o)});if(!a.length){let o=document.createElement('option');o.textContent='未找到网络，请稍后刷新';s.appendChild(o)}}).catch(()=>{});</script></body></html>";

static esp_err_t load_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("chronowifi", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_password);
    size_t city_len = sizeof(s_city);
    int16_t timezone = 480;
    err = nvs_get_str(handle, "ssid", s_ssid, &ssid_len);
    if (err == ESP_OK) err = nvs_get_str(handle, "pass", s_password, &pass_len);
    if (nvs_get_str(handle, "city", s_city, &city_len) != ESP_OK) s_city[0] = '\0';
    if (nvs_get_i16(handle, "tz_min", &timezone) == ESP_OK &&
        timezone >= -720 && timezone <= 840) {
        s_timezone_minutes = timezone;
    }
    nvs_close(handle);
    if (err == ESP_OK && s_ssid[0] != '\0') s_has_credentials = true;
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t save_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("chronowifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_str(handle, "ssid", s_ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, "pass", s_password)) == ESP_OK &&
        (err = nvs_set_str(handle, "city", s_city)) == ESP_OK &&
        (err = nvs_set_i16(handle, "tz_min", s_timezone_minutes)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void time_sync_cb(struct timeval *tv)
{
    if (tv == NULL || tv->tv_sec < 946684800) return;
    portENTER_CRITICAL(&s_time_lock);
    s_pending_unix = tv->tv_sec;
    s_time_pending = true;
    portEXIT_CRITICAL(&s_time_lock);
    ESP_LOGI(TAG, "SNTP time received; RTC update queued");
}

static void start_sntp(void)
{
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.cloudflare.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
}

static void start_station(void)
{
    if (!s_enabled) {
        s_state = WIFI_SETUP_UNCONFIGURED;
        return;
    }
    if (!s_has_credentials) {
        s_state = WIFI_SETUP_UNCONFIGURED;
        return;
    }
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, s_ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, s_password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &config));
    s_retry_count = 0;
    s_state = WIFI_SETUP_CONNECTING;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    ESP_LOGI(TAG, "connecting to saved Wi-Fi SSID %s", s_ssid);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED &&
        s_enabled && !s_portal_running) {
        if (s_scanning) {
            // A foreground scan can briefly disturb the current channel. Let
            // the scan finish before reconnecting so the driver does not run
            // two allocation-heavy operations at once.
            s_disconnected_during_scan = true;
            return;
        }
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, retry=%d/%d",
                 event ? (int)event->reason : -1, s_retry_count,
                 WIFI_RETRY_LIMIT);
        if (s_retry_count++ < WIFI_RETRY_LIMIT) {
            s_state = WIFI_SETUP_CONNECTING;
            esp_wifi_connect();
        } else {
            s_state = WIFI_SETUP_ERROR;
            ESP_LOGW(TAG, "saved Wi-Fi connection failed after retries");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_state = WIFI_SETUP_CONNECTED;
        start_sntp();
        ESP_LOGI(TAG, "Wi-Fi connected; background SNTP enabled");
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, size_t capacity, const char *src, size_t length)
{
    size_t out = 0;
    for (size_t i = 0; i < length && out + 1 < capacity; i++) {
        if (src[i] == '+') dst[out++] = ' ';
        else if (src[i] == '%' && i + 2 < length) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else dst[out++] = src[i];
    }
    dst[out] = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t capacity)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;
    while (cursor && *cursor) {
        if ((cursor == body || cursor[-1] == '&') &&
            strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value = cursor + key_len + 1;
            const char *end = strchr(value, '&');
            url_decode(out, capacity, value, end ? (size_t)(end - value) : strlen(value));
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor) cursor++;
    }
    return false;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    uint16_t count = 0;
    wifi_scan_config_t scan = {0};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err == ESP_OK) esp_wifi_scan_get_ap_num(&count);
    if (count > 12) count = 12;
    wifi_ap_record_t records[12] = {0};
    if (count > 0) esp_wifi_scan_get_ap_records(&count, records);
    char json[768];
    size_t used = 0;
    json[used++] = '[';
    for (uint16_t i = 0; i < count && used + 40 < sizeof(json); i++) {
        if (records[i].ssid[0] == '\0') continue;
        if (used > 1) json[used++] = ',';
        json[used++] = '"';
        for (size_t j = 0; records[i].ssid[j] && used + 3 < sizeof(json); j++) {
            const char c = (char)records[i].ssid[j];
            if (c == '"' || c == '\\') json[used++] = '\\';
            json[used++] = c;
        }
        json[used++] = '"';
    }
    json[used++] = ']';
    json[used] = '\0';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, used);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    char body[513];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) return ESP_FAIL;
    body[received] = '\0';
    char timezone[12] = "480";
    if (!form_value(body, "ssid", s_ssid, sizeof(s_ssid)) || s_ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }
    form_value(body, "password", s_password, sizeof(s_password));
    form_value(body, "city", s_city, sizeof(s_city));
    form_value(body, "tz", timezone, sizeof(timezone));
    long tz = strtol(timezone, NULL, 10);
    if (tz >= -720 && tz <= 840) s_timezone_minutes = (int16_t)tz;
    const esp_err_t err = save_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving Wi-Fi credentials failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }
    s_has_credentials = true;
    s_connect_after_portal = true;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
        "<body style='background:#05080d;color:white;font:20px -apple-system;padding:40px'>"
        "<h2>配置已保存</h2><p>时迹正在连接网络，此页面可以关闭。</p></body>");
}

static void dns_server(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    uint8_t packet[512];
    while (s_portal_running) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int length = recvfrom(fd, packet, sizeof(packet), 0,
                              (struct sockaddr *)&source, &source_len);
        if (length < 12) continue;
        packet[2] |= 0x80;
        packet[3] = 0x80;
        packet[7] = 1;
        if (length + 16 > (int)sizeof(packet)) continue;
        int out = length;
        packet[out++] = 0xC0; packet[out++] = 0x0C;
        packet[out++] = 0x00; packet[out++] = 0x01;
        packet[out++] = 0x00; packet[out++] = 0x01;
        packet[out++] = 0; packet[out++] = 0; packet[out++] = 0; packet[out++] = 30;
        packet[out++] = 0; packet[out++] = 4;
        packet[out++] = 192; packet[out++] = 168; packet[out++] = 4; packet[out++] = 1;
        sendto(fd, packet, out, 0, (struct sockaddr *)&source, source_len);
    }
    close(fd);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void stop_portal(void)
{
    s_portal_running = false;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();
}

static void control_task(void *arg)
{
    (void)arg;
    TickType_t portal_started = 0;
    for (;;) {
        if (s_portal_running && portal_started == 0) portal_started = xTaskGetTickCount();
        if (!s_portal_running) portal_started = 0;
        if (s_connect_after_portal) {
            s_connect_after_portal = false;
            vTaskDelay(pdMS_TO_TICKS(600));
            stop_portal();
            start_station();
        } else if (s_portal_running && portal_started != 0 &&
                   xTaskGetTickCount() - portal_started > pdMS_TO_TICKS(PORTAL_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "configuration portal timed out");
            stop_portal();
            if (s_has_credentials) start_station();
            else s_state = WIFI_SETUP_UNCONFIGURED;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t wifi_setup_init(bool enabled)
{
    if (s_wifi_ready) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return ESP_FAIL;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&config)) != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                             wifi_event, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                             wifi_event, NULL));
    s_wifi_ready = true;
    s_enabled = enabled;
    load_credentials();
    xTaskCreate(control_task, "wifi_ctl", 4096, NULL, 3, &s_control_task);
    if (s_enabled && s_has_credentials) start_station();
    return ESP_OK;
}

esp_err_t wifi_setup_set_enabled(bool enabled)
{
    if (!s_wifi_ready) return ESP_ERR_INVALID_STATE;
    if (s_enabled == enabled) return ESP_OK;
    s_enabled = enabled;
    if (!enabled) {
        if (esp_sntp_enabled()) esp_sntp_stop();
        // Keep the initialized driver and its DMA-capable buffers allocated.
        // Releasing them here makes a later start fail on the fragmented
        // internal heap once BLE, audio, and rendering are all active.
        s_scanning = true;
        esp_wifi_disconnect();
        s_scanning = false;
        s_retry_count = 0;
        s_state = WIFI_SETUP_UNCONFIGURED;
        ESP_LOGI(TAG, "Wi-Fi disabled from settings");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Wi-Fi enabled from settings");
    if (s_has_credentials) start_station();
    else s_state = WIFI_SETUP_UNCONFIGURED;
    return ESP_OK;
}

bool wifi_setup_enabled(void)
{
    return s_enabled;
}

esp_err_t wifi_setup_start_portal(void)
{
    if (!s_wifi_ready) return ESP_ERR_INVALID_STATE;
    if (s_portal_running) return ESP_OK;
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_wifi_stop();
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, PORTAL_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, PORTAL_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(PORTAL_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set portal AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start portal AP");
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &http), TAG, "start portal HTTP");
    const httpd_uri_t root = {.uri = "/*", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &root);
    s_portal_running = true;
    s_state = WIFI_SETUP_PORTAL;
    xTaskCreate(dns_server, "wifi_dns", 3072, NULL, 3, &s_dns_task);
    ESP_LOGI(TAG, "configuration portal ready: SSID=%s IP=192.168.4.1", PORTAL_SSID);
    return ESP_OK;
}

wifi_setup_state_t wifi_setup_state(void)
{
    return s_state;
}

bool wifi_setup_has_credentials(void)
{
    return s_has_credentials;
}

esp_err_t wifi_setup_scan(char (*ssids)[33], size_t capacity, size_t *count)
{
    if (!s_wifi_ready || !ssids || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    const bool was_enabled = s_enabled;
    const wifi_setup_state_t previous_state = s_state;
    s_scanning = true;
    s_disconnected_during_scan = false;

    // Scanning while already connected is supported by the ESP-IDF station
    // driver. Keep that driver and its allocations alive: stopping and
    // immediately restarting it caused memory churn and reconnect failures on
    // the Wi-Fi + BLE build. Only start a temporary station when Wi-Fi was off.
    esp_err_t err = ESP_OK;
    if (!was_enabled || previous_state == WIFI_SETUP_UNCONFIGURED) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_start();
    }
    wifi_scan_config_t scan = {0};
    if (err == ESP_OK) err = esp_wifi_scan_start(&scan, true);
    uint16_t found = 0;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_num(&found);
    if (found > 20) found = 20;
    wifi_ap_record_t records[20] = {0};
    if (err == ESP_OK && found > 0) err = esp_wifi_scan_get_ap_records(&found, records);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < found && *count < capacity; i++) {
            if (records[i].ssid[0] == '\0') continue;
            bool duplicate = false;
            for (size_t j = 0; j < *count; j++) {
                if (strcmp(ssids[j], (const char *)records[i].ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                strlcpy(ssids[*count], (const char *)records[i].ssid, 33);
                (*count)++;
            }
        }
    }
    s_scanning = false;
    if (!was_enabled) {
        // The scan may have started the station driver. Leave its buffers in
        // place for the next On tap, but end all radio association activity.
        esp_wifi_disconnect();
        s_state = WIFI_SETUP_UNCONFIGURED;
    } else if (s_has_credentials &&
               (s_disconnected_during_scan || previous_state != WIFI_SETUP_CONNECTED)) {
        s_retry_count = 0;
        s_state = WIFI_SETUP_CONNECTING;
        const esp_err_t connect_err = esp_wifi_connect();
        if (err == ESP_OK && connect_err != ESP_OK) err = connect_err;
    } else if (previous_state == WIFI_SETUP_CONNECTED) {
        s_state = WIFI_SETUP_CONNECTED;
    }
    ESP_LOGI(TAG, "device UI scan found %u networks", (unsigned)*count);
    return err;
}

esp_err_t wifi_setup_connect(const char *ssid, const char *password,
                             int16_t timezone_minutes)
{
    if (!s_wifi_ready || !ssid || ssid[0] == '\0' || strlen(ssid) > 32 ||
        !password || strlen(password) > 64 ||
        timezone_minutes < -720 || timezone_minutes > 840) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_password, password, sizeof(s_password));
    s_timezone_minutes = timezone_minutes;
    esp_err_t err = save_credentials();
    if (err != ESP_OK) return err;
    s_has_credentials = true;
    s_enabled = true;
    if (s_portal_running) {
        stop_portal();
        start_station();
        return ESP_OK;
    }

    // The device-side editor is entered from an already running STA driver.
    // Stopping and immediately restarting that driver races its asynchronous
    // DISCONNECTED event and can leave the newly saved credentials idle until
    // the next reboot. Reconfigure the live STA instead, suppressing the
    // intentional disconnect from the retry handler.
    if (esp_sntp_enabled()) esp_sntp_stop();
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, s_ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, s_password,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    s_scanning = true;
    esp_wifi_disconnect();
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    s_retry_count = 0;
    s_state = WIFI_SETUP_CONNECTING;
    if (err == ESP_OK) err = esp_wifi_connect();
    s_scanning = false;
    if (err != ESP_OK) {
        s_state = WIFI_SETUP_ERROR;
        ESP_LOGE(TAG, "live Wi-Fi reconfiguration failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "connecting with updated device-UI credentials: %s", s_ssid);
    return ESP_OK;
}

bool wifi_setup_take_time(int64_t *unix_seconds, int16_t *timezone_minutes)
{
    if (!unix_seconds || !timezone_minutes) return false;
    bool available = false;
    portENTER_CRITICAL(&s_time_lock);
    if (s_time_pending) {
        *unix_seconds = s_pending_unix;
        *timezone_minutes = s_timezone_minutes;
        s_time_pending = false;
        available = true;
    }
    portEXIT_CRITICAL(&s_time_lock);
    return available;
}
