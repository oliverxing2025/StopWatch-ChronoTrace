#include "ble_setup.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "services/hid/ble_svc_hid.h"
#include "os/os_mbuf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "battery.h"

static const char *TAG = "ble_setup";
static volatile ble_setup_state_t s_state;
static uint8_t s_own_addr_type;
static bool s_initialized;
static bool s_enabled;
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_pending;
static int64_t s_unix_seconds;
static int16_t s_timezone_minutes;
static uint16_t s_cts_start_handle;
static uint16_t s_cts_end_handle;
static uint16_t s_current_time_handle;
static uint16_t s_local_time_handle;
static int16_t s_cts_timezone_minutes;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_time_request_pending;

void ble_store_config_init(void);

#define BLE_UUID_CTS_SERVICE            0x1805
#define BLE_UUID_CURRENT_TIME           0x2A2B
#define BLE_UUID_LOCAL_TIME_INFORMATION 0x2A0F

static const ble_uuid16_t s_hid_service_uuid = BLE_UUID16_INIT(0x1812);

// 8f62xxxx-7b9d-4b8a-a3c4-435452414345 ("CTRACE")
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x45,0x43,0x41,0x52,0x54,0x43,0xc4,0xa3,
                     0x8a,0x4b,0x9d,0x7b,0x00,0x00,0x62,0x8f);
static const ble_uuid128_t s_time_uuid =
    BLE_UUID128_INIT(0x45,0x43,0x41,0x52,0x54,0x43,0xc4,0xa3,
                     0x8a,0x4b,0x9d,0x7b,0x01,0x00,0x62,0x8f);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle;
    (void)arg;
    const int length = OS_MBUF_PKTLEN(ctxt->om);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (length != 10) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t data[10];
    if (ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), NULL) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint64_t seconds = 0;
    for (int i = 0; i < 8; i++) seconds |= (uint64_t)data[i] << (i * 8);
    const int16_t timezone = (int16_t)((uint16_t)data[8] | ((uint16_t)data[9] << 8));
    portENTER_CRITICAL(&s_data_lock);
    s_unix_seconds = (int64_t)seconds;
    s_timezone_minutes = timezone;
    s_time_pending = true;
    s_time_request_pending = false;
    portEXIT_CRITICAL(&s_data_lock);
    ESP_LOGI(TAG, "phone time packet received");
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &s_time_uuid.u, .access_cb = gatt_access,
             .flags = BLE_GATT_CHR_F_WRITE},
            {0}
        },
    },
    {0}
};

static int add_pairing_hid_service(void)
{
    // A real, inert vendor-defined HID service makes ChronoTrace eligible for
    // the iPhone system Bluetooth picker. It never emits keyboard, pointer or
    // media-key input; HID is used only as the standards-compliant pairing
    // shell around the existing time-calibration GATT service.
    static const uint8_t report_map[] = {
        0x06, 0x00, 0xff,       // Usage Page (Vendor Defined 0xFF00)
        0x09, 0x01,             // Usage (1)
        0xa1, 0x01,             // Collection (Application)
        0x85, 0x01,             //   Report ID (1)
        0x15, 0x00,             //   Logical Minimum (0)
        0x26, 0xff, 0x00,       //   Logical Maximum (255)
        0x75, 0x08,             //   Report Size (8)
        0x95, 0x01,             //   Report Count (1)
        0x09, 0x01,             //   Usage (1)
        0x81, 0x02,             //   Input (Data,Var,Abs)
        0xc0                    // End Collection
    };
    struct ble_svc_hid_params hid = {0};
    hid.proto_mode_present = 1;
    hid.proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;
    memcpy(hid.report_map, report_map, sizeof(report_map));
    hid.report_map_len = sizeof(report_map);
    // HID 1.11, no country code, normally connectable.
    hid.hid_info = 0x02000111U;
    hid.external_rpt_ref = BLE_SVC_BAS_UUID16;
    hid.rpts_len = 1;
    hid.rpts[0].id = 1;
    hid.rpts[0].type = BLE_SVC_HID_RPT_TYPE_INPUT;
    hid.rpts[0].len = 1;

    const int rc = ble_svc_hid_add(hid);
    if (rc != 0) return rc;
    ble_svc_bas_init();
    uint8_t percent = 0;
    uint16_t millivolts = 0;
    if (battery_read(&percent, &millivolts) == ESP_OK) {
        ble_svc_bas_battery_level_set(percent);
    }
    ble_svc_hid_init();
    return 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void publish_cts_time(const uint8_t data[10])
{
    const int year = data[0] | ((int)data[1] << 8);
    const unsigned month = data[2];
    const unsigned day = data[3];
    const unsigned hour = data[4];
    const unsigned minute = data[5];
    const unsigned second = data[6];
    if (year < 2000 || year > 2199 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        ESP_LOGW(TAG, "invalid Current Time value from phone");
        return;
    }
    const int64_t local_seconds = days_from_civil(year, month, day) * 86400 +
                                  hour * 3600 + minute * 60 + second;
    portENTER_CRITICAL(&s_data_lock);
    s_timezone_minutes = s_cts_timezone_minutes;
    s_unix_seconds = local_seconds - (int64_t)s_timezone_minutes * 60;
    s_time_pending = true;
    s_time_request_pending = false;
    portEXIT_CRITICAL(&s_data_lock);
    ESP_LOGI(TAG, "iPhone Current Time received: %04d-%02u-%02u %02u:%02u:%02u, tz=%d",
             year, month, day, hour, minute, second, s_cts_timezone_minutes);
}

static int current_time_read_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status != 0 || attr == NULL) {
        ESP_LOGW(TAG, "Current Time read failed: %d", error->status);
        return 0;
    }
    uint8_t data[10];
    if (OS_MBUF_PKTLEN(attr->om) >= sizeof(data) &&
        ble_hs_mbuf_to_flat(attr->om, data, sizeof(data), NULL) == 0) {
        publish_cts_time(data);
    }
    return 0;
}

static void read_current_time(uint16_t conn_handle)
{
    if (s_current_time_handle == 0) {
        ESP_LOGW(TAG, "iPhone did not expose Current Time characteristic");
        return;
    }
    const int rc = ble_gattc_read(conn_handle, s_current_time_handle,
                                  current_time_read_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "Current Time read start failed: %d", rc);
}

static int local_time_read_cb(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (error->status == 0 && attr != NULL && OS_MBUF_PKTLEN(attr->om) >= 2) {
        uint8_t data[2];
        if (ble_hs_mbuf_to_flat(attr->om, data, sizeof(data), NULL) == 0) {
            const int8_t quarter_hours = (int8_t)data[0];
            if (quarter_hours >= -48 && quarter_hours <= 56) {
                s_cts_timezone_minutes = quarter_hours * 15;
            }
        }
    }
    read_current_time(conn_handle);
    return 0;
}

static int cts_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        const uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == BLE_UUID_CURRENT_TIME) s_current_time_handle = chr->val_handle;
        else if (uuid == BLE_UUID_LOCAL_TIME_INFORMATION) s_local_time_handle = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "CTS characteristic discovery failed: %d", error->status);
        return 0;
    }
    if (s_local_time_handle != 0) {
        const int rc = ble_gattc_read(conn_handle, s_local_time_handle,
                                      local_time_read_cb, NULL);
        if (rc == 0) return 0;
        ESP_LOGW(TAG, "Local Time read start failed: %d", rc);
    }
    read_current_time(conn_handle);
    return 0;
}

static int cts_service_cb(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_cts_start_handle = service->start_handle;
        s_cts_end_handle = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || s_cts_start_handle == 0) {
        ESP_LOGW(TAG, "iPhone Current Time Service unavailable: %d", error->status);
        return 0;
    }
    s_current_time_handle = 0;
    s_local_time_handle = 0;
    const int rc = ble_gattc_disc_all_chrs(conn_handle,
                                           s_cts_start_handle + 1,
                                           s_cts_end_handle,
                                           cts_chr_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "CTS characteristic discovery start failed: %d", rc);
    return 0;
}

static void discover_phone_time(uint16_t conn_handle)
{
    s_cts_start_handle = 0;
    s_cts_end_handle = 0;
    s_cts_timezone_minutes = 0;
    const int rc = ble_gattc_disc_svc_by_uuid(
        conn_handle, BLE_UUID16_DECLARE(BLE_UUID_CTS_SERVICE),
        cts_service_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "CTS discovery start failed: %d", rc);
}

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_state = BLE_SETUP_CONNECTED;
                ESP_LOGI(TAG, "phone connected, handle=%u",
                         (unsigned)event->connect.conn_handle);
                const int rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "pairing request failed: %d", rc);
                }
            } else {
                ESP_LOGW(TAG, "connection failed, status=%d",
                         event->connect.status);
                advertise();
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption changed, status=%d", event->enc_change.status);
            if (event->enc_change.status == 0 && s_time_request_pending) {
                discover_phone_time(event->enc_change.conn_handle);
            }
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "phone disconnected, reason=%d",
                     event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_time_request_pending = false;
            if (s_enabled) advertise();
            else s_state = BLE_SETUP_OFF;
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_enabled) advertise();
            else s_state = BLE_SETUP_OFF;
            break;
        default:
            break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // Keep both the HID identity and complete device name in the primary
    // packet. iOS Settings can filter before requesting scan-response data.
    fields.uuids16 = &s_hid_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = BLE_SVC_GAP_APPEARANCE_GEN_HID;
    fields.appearance_is_present = 1;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertisement metadata failed: %d", rc);
        s_state = BLE_SETUP_ERROR;
        return;
    }

    struct ble_hs_adv_fields response = {0};
    // The scan response advertises the actual ChronoTrace calibration service
    // for app-level scanners without crowding the iOS pairing identity.
    response.uuids128 = &s_service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response data failed: %d", rc);
        s_state = BLE_SETUP_ERROR;
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 20 ms is Apple's recommended fast-discovery interval for the initial
    // pairing window (units are 0.625 ms).
    params.itvl_min = 32;
    params.itvl_max = 32;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
        s_state = BLE_SETUP_ERROR;
        return;
    }
    s_state = BLE_SETUP_ADVERTISING;
    ESP_LOGI(TAG, "advertising as ChronoTrace");
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        s_state = BLE_SETUP_ERROR;
        return;
    }
    if (s_enabled) advertise();
    else s_state = BLE_SETUP_OFF;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
    s_state = BLE_SETUP_STARTING;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_setup_start(void)
{
    s_enabled = true;
    if (s_initialized) return ble_setup_make_discoverable();
    s_state = BLE_SETUP_STARTING;
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        s_state = BLE_SETUP_ERROR;
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_services);
    if (rc == 0) rc = add_pairing_hid_service();
    if (rc != 0) {
        nimble_port_deinit();
        s_state = BLE_SETUP_ERROR;
        ESP_LOGE(TAG, "GATT service setup failed: %d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set("ChronoTrace");
    ble_svc_gap_device_appearance_set(BLE_SVC_GAP_APPEARANCE_GEN_HID);
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ble_setup_stop(void)
{
    if (!s_initialized) return ESP_OK;
    s_enabled = false;
    if (s_state == BLE_SETUP_ADVERTISING) ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "NimBLE stop failed: %d", rc);
        return ESP_FAIL;
    }
    const esp_err_t err = nimble_port_deinit();
    if (err == ESP_OK) {
        ble_svc_hid_reset();
        s_initialized = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_time_request_pending = false;
        s_state = BLE_SETUP_OFF;
    }
    return err;
}

esp_err_t ble_setup_set_enabled(bool enabled)
{
    if (enabled) {
        s_enabled = true;
        return s_initialized ? ble_setup_make_discoverable() : ble_setup_start();
    }
    s_enabled = false;
    s_time_request_pending = false;
    if (!s_initialized) {
        s_state = BLE_SETUP_OFF;
        return ESP_OK;
    }
    if (ble_gap_adv_active()) {
        const int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "advertising suspend failed: %d", rc);
            return ESP_FAIL;
        }
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(s_conn_handle,
                                         BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "connection suspend failed: %d", rc);
            return ESP_FAIL;
        }
    }
    s_state = BLE_SETUP_OFF;
    ESP_LOGI(TAG, "Bluetooth suspended; host memory retained");
    return ESP_OK;
}

esp_err_t ble_setup_make_discoverable(void)
{
    if (!s_initialized) return ble_setup_start();
    if (s_state == BLE_SETUP_CONNECTED ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_OK;
    }

    // ble_setup_start() is intentionally idempotent, but that alone cannot
    // recover a stale or completed advertisement. A user pressing Connect is
    // an explicit request for a fresh discoverable window, so restart it even
    // when the host was already initialized at boot.
    if (ble_gap_adv_active()) {
        const int stop_rc = ble_gap_adv_stop();
        if (stop_rc != 0 && stop_rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "advertising refresh stop failed: %d", stop_rc);
        }
    }
    s_state = BLE_SETUP_STARTING;
    advertise();
    if (s_state != BLE_SETUP_ADVERTISING) return ESP_FAIL;
    ESP_LOGI(TAG, "connect button refreshed discoverable advertising");
    return ESP_OK;
}

ble_setup_state_t ble_setup_state(void)
{
    return s_state;
}

esp_err_t ble_setup_request_time(void)
{
    if (!s_initialized || s_state != BLE_SETUP_CONNECTED ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_time_request_pending = true;
    struct ble_gap_conn_desc desc;
    const int find_rc = ble_gap_conn_find(s_conn_handle, &desc);
    if (find_rc != 0) {
        s_time_request_pending = false;
        return ESP_FAIL;
    }
    if (desc.sec_state.encrypted) {
        discover_phone_time(s_conn_handle);
        return ESP_OK;
    }
    const int rc = ble_gap_security_initiate(s_conn_handle);
    if (rc == 0 || rc == BLE_HS_EALREADY) return ESP_OK;
    s_time_request_pending = false;
    ESP_LOGW(TAG, "time request could not secure connection: %d", rc);
    return ESP_FAIL;
}

bool ble_setup_take_time(int64_t *unix_seconds, int16_t *timezone_minutes)
{
    if (!unix_seconds || !timezone_minutes) return false;
    bool available;
    portENTER_CRITICAL(&s_data_lock);
    available = s_time_pending;
    if (available) {
        *unix_seconds = s_unix_seconds;
        *timezone_minutes = s_timezone_minutes;
        s_time_pending = false;
    }
    portEXIT_CRITICAL(&s_data_lock);
    return available;
}
