#include "vibe_ble_mouse.h"

#if defined(VIBE_BOARD_STICKC_PLUS)
#include <inttypes.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "vibe_ble_mouse";
static esp_hidd_dev_t *s_hid;
static bool s_started;
static bool s_adv_ready;
static bool s_stack_configured;

static const uint8_t k_mouse_report_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
    0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};
static esp_hid_raw_report_map_t s_report_maps[] = {{.data = k_mouse_report_map, .len = sizeof(k_mouse_report_map)}};
static esp_hid_device_config_t s_config = {
    .vendor_id = 0x16c0, .product_id = 0x05df, .version = 0x0100,
    .device_name = "VibeStick MiniJoy Mouse", .manufacturer_name = "VibeStick",
    .serial_number = "MiniJoyC", .report_maps = s_report_maps, .report_maps_len = 1,
};
static esp_ble_adv_params_t s_adv = {
    .adv_int_min = 0x20, .adv_int_max = 0x30, .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void maybe_advertise(void)
{
    if (s_started && s_adv_ready && s_hid && !esp_hidd_dev_connected(s_hid)) {
        esp_err_t err = esp_ble_gap_start_advertising(&s_adv);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "advertise: %s", esp_err_to_name(err));
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) { s_adv_ready = true; maybe_advertise(); }
    if (event == ESP_GAP_BLE_AUTH_CMPL_EVT && !param->ble_security.auth_cmpl.success) ESP_LOGW(TAG, "bond failed");
}

static void hid_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == ESP_HIDD_START_EVENT || id == ESP_HIDD_DISCONNECT_EVENT) maybe_advertise();
}

static esp_err_t configure_stack_once(void)
{
    if (s_stack_configured) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_cb), TAG, "gap callback");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler), TAG,
                        "gatts callback");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(s_config.device_name), TAG, "device name");

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                                        &auth_req, sizeof(auth_req)), TAG,
                        "bond auth");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                                        &io_cap, sizeof(io_cap)), TAG,
                        "bond io capability");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                                        &key_size, sizeof(key_size)), TAG,
                        "bond key size");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                                        &init_key, sizeof(init_key)), TAG,
                        "bond initiator keys");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                                        &rsp_key, sizeof(rsp_key)), TAG,
                        "bond responder keys");
    s_stack_configured = true;
    return ESP_OK;
}

esp_err_t vibe_ble_mouse_start(void)
{
    if (s_started) return ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_bt_controller_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller init failed: %s free_heap=%" PRIu32,
                     esp_err_to_name(err), esp_get_free_heap_size());
            return err;
        }
        ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller enable");
        ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init");
        ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");
    }
    ESP_RETURN_ON_ERROR(configure_stack_once(), TAG, "configure BLE stack");
    esp_ble_adv_data_t adv = {.set_scan_rsp = false, .include_name = true, .include_txpower = true,
        .appearance = ESP_HID_APPEARANCE_MOUSE, .flag = 0x6};
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&adv), TAG, "adv data");
    s_adv_ready = false;
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hid_cb, &s_hid), TAG, "hid init");
    s_started = true;
    maybe_advertise();
    return ESP_OK;
}

void vibe_ble_mouse_stop(void)
{
    if (!s_started) return;
    (void)vibe_ble_mouse_report(0, 0, false);
    (void)esp_ble_gap_stop_advertising();
    if (s_hid) (void)esp_hidd_dev_deinit(s_hid);
    s_hid = NULL;
    s_started = false;
    s_adv_ready = false;
}

bool vibe_ble_mouse_connected(void) { return s_hid && esp_hidd_dev_connected(s_hid); }

esp_err_t vibe_ble_mouse_report(int8_t dx, int8_t dy, bool left_pressed)
{
    if (!s_hid || !esp_hidd_dev_connected(s_hid)) return ESP_ERR_INVALID_STATE;
    uint8_t report[] = {left_pressed ? 1 : 0, (uint8_t)dx, (uint8_t)dy};
    return esp_hidd_dev_input_set(s_hid, 0, 0, report, sizeof(report));
}
#else
esp_err_t vibe_ble_mouse_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void vibe_ble_mouse_stop(void) {}
bool vibe_ble_mouse_connected(void) { return false; }
esp_err_t vibe_ble_mouse_report(int8_t dx, int8_t dy, bool left_pressed)
{
    (void)dx;
    (void)dy;
    (void)left_pressed;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
