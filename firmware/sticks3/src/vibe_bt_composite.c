#include "vibe_bt_composite.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hidd.h"
#include "esp_hidd_api.h"
#include "esp_hid_common.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HID_REPORT_ID_KEYBOARD 1
#define HID_REPORT_ID_MOUSE 2
#define AUDIO_PUMP_PERIOD_MS 8
#define RECONNECT_TASK_PERIOD_MS 250
#define RECONNECT_REQUEST_GAP_MS 1000
#define RECONNECT_INITIAL_DELAY_MS 750
#define RECONNECT_ATTEMPT_TIMEOUT_MS 8000
#define RECONNECT_BACKOFF_MAX_MS 30000

static const char *TAG = "vibe_bt_composite";
static const char *DEVICE_NAME = "VibeStick MiniJoy";

static esp_hidd_dev_t *s_hid;
static vibe_bt_composite_state_t s_state;
static vibe_bt_state_callback_t s_state_callback;
static void *s_state_context;
static vibe_bt_pcm_read_fn s_pcm_reader;
static void *s_pcm_context;
static TaskHandle_t s_audio_pump_task;
static TaskHandle_t s_reconnect_task;
static char s_serial[20];
static esp_bd_addr_t s_reconnect_address;
static portMUX_TYPE s_reconnect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_reconnect_target_valid;
static bool s_hid_ready;
static bool s_hid_connecting;
static bool s_hfp_ready;
static bool s_hfp_connecting;
static int64_t s_hid_retry_at_ms;
static int64_t s_hfp_retry_at_ms;
static int64_t s_last_reconnect_request_ms;
static uint32_t s_hid_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
static uint32_t s_hfp_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t next_retry_delay(uint32_t current)
{
    uint32_t doubled = current * 2;
    return doubled > RECONNECT_BACKOFF_MAX_MS
               ? RECONNECT_BACKOFF_MAX_MS
               : doubled;
}

static void schedule_hid_retry(int64_t current_ms, bool reset_backoff)
{
    if (reset_backoff) {
        s_hid_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
    }
    s_hid_retry_at_ms = current_ms + s_hid_retry_delay_ms;
    s_hid_retry_delay_ms = next_retry_delay(s_hid_retry_delay_ms);
}

static void schedule_hfp_retry(int64_t current_ms, bool reset_backoff)
{
    if (reset_backoff) {
        s_hfp_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
    }
    s_hfp_retry_at_ms = current_ms + s_hfp_retry_delay_ms;
    s_hfp_retry_delay_ms = next_retry_delay(s_hfp_retry_delay_ms);
}

static const uint8_t k_report_map[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xa1, 0x01,       // Collection (Application)
    0x85, HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,       // Usage Page (Keyboard)
    0x19, 0xe0,       // Usage Minimum (Left Control)
    0x29, 0xe7,       // Usage Maximum (Right GUI)
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,       // Input (modifier byte)
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,       // Input (reserved)
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,       // Input (six key slots)
    0xc0,

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xa1, 0x01,
    0x85, HID_REPORT_ID_MOUSE,
    0x09, 0x01,
    0xa1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x03,
    0x81, 0x02,
    0x75, 0x05,
    0x95, 0x01,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7f,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xc0,
    0xc0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = k_report_map, .len = sizeof(k_report_map)},
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16c0,
    .product_id = 0x05df,
    .version = 0x0200,
    .device_name = "VibeStick MiniJoy",
    .manufacturer_name = "VibeStick",
    .serial_number = s_serial,
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static void notify_state(void)
{
    if (s_state_callback) {
        s_state_callback(&s_state, s_state_context);
    }
}

static void set_scan_mode(bool pairing)
{
    s_state.pairing = pairing;
    esp_err_t err = esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE,
        pairing ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set scan mode failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "scan mode pairing=%d", pairing);
    }
    notify_state();
}

static void set_reconnect_target(const esp_bd_addr_t address)
{
    int64_t current_ms = now_ms();
    portENTER_CRITICAL(&s_reconnect_lock);
    memcpy(s_reconnect_address, address, sizeof(s_reconnect_address));
    s_reconnect_target_valid = true;
    s_hid_connecting = false;
    s_hfp_connecting = false;
    s_hid_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
    s_hfp_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
    s_hid_retry_at_ms = current_ms + RECONNECT_INITIAL_DELAY_MS;
    s_hfp_retry_at_ms = current_ms + RECONNECT_INITIAL_DELAY_MS;
    portEXIT_CRITICAL(&s_reconnect_lock);
}

static void gap_callback(esp_bt_gap_cb_event_t event,
                         esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            s_state.paired = true;
            set_reconnect_target(param->auth_cmpl.bda);
            set_scan_mode(false);
            ESP_LOGI(TAG, "bonded with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "authentication failed status=%d",
                     param->auth_cmpl.stat);
        }
        notify_state();
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;
    }
    default:
        break;
    }
}

static void hid_callback(void *arg, esp_event_base_t base, int32_t id,
                         void *event_data)
{
    (void)arg;
    (void)base;
    esp_hidd_event_data_t *data = event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        if (data && data->start.status != ESP_OK) {
            ESP_LOGE(TAG, "HID start failed: %s",
                     esp_err_to_name(data->start.status));
        } else {
            portENTER_CRITICAL(&s_reconnect_lock);
            s_hid_ready = true;
            s_hid_retry_at_ms = now_ms() + RECONNECT_INITIAL_DELAY_MS;
            portEXIT_CRITICAL(&s_reconnect_lock);
        }
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_state.hid_connected = !data || data->connect.status == ESP_OK;
        portENTER_CRITICAL(&s_reconnect_lock);
        s_hid_connecting = false;
        if (s_state.hid_connected) {
            s_hid_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
        } else {
            schedule_hid_retry(now_ms(), false);
        }
        portEXIT_CRITICAL(&s_reconnect_lock);
        ESP_LOGI(TAG, "HID connected=%d", s_state.hid_connected);
        notify_state();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_state.hid_connected = false;
        portENTER_CRITICAL(&s_reconnect_lock);
        s_hid_connecting = false;
        schedule_hid_retry(now_ms(), true);
        portEXIT_CRITICAL(&s_reconnect_lock);
        ESP_LOGI(TAG, "HID disconnected; automatic reconnect scheduled");
        notify_state();
        break;
    default:
        break;
    }
}

static void hfp_callback(esp_hf_client_cb_event_t event,
                         esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (param->prof_stat.state == ESP_HF_INIT_SUCCESS) {
            portENTER_CRITICAL(&s_reconnect_lock);
            s_hfp_ready = true;
            s_hfp_retry_at_ms = now_ms() + RECONNECT_INITIAL_DELAY_MS;
            portEXIT_CRITICAL(&s_reconnect_lock);
        }
        break;
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        s_state.hfp_connected =
            param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED;
        portENTER_CRITICAL(&s_reconnect_lock);
        s_hfp_connecting =
            param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING;
        if (s_state.hfp_connected) {
            s_hfp_retry_delay_ms = RECONNECT_INITIAL_DELAY_MS;
        } else if (param->conn_stat.state ==
                   ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_hfp_connecting = false;
            schedule_hfp_retry(now_ms(), true);
            s_state.audio_connected = false;
            s_state.wideband = false;
        }
        portEXIT_CRITICAL(&s_reconnect_lock);
        ESP_LOGI(TAG, "HFP state=%d connected=%d",
                 param->conn_stat.state, s_state.hfp_connected);
        notify_state();
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        s_state.audio_connected =
            param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC;
        s_state.wideband =
            param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC;
        ESP_LOGI(TAG, "HFP audio connected=%d codec=%s frame=%u",
                 s_state.audio_connected, s_state.wideband ? "mSBC" : "CVSD",
                 param->audio_stat.preferred_frame_size);
        notify_state();
        break;
    default:
        break;
    }
}

static void hfp_incoming_audio(const uint8_t *buffer, uint32_t length)
{
    (void)buffer;
    (void)length;
}

static uint32_t hfp_outgoing_audio(uint8_t *buffer, uint32_t length)
{
    if (!buffer || length == 0 || !s_pcm_reader) {
        return 0;
    }
    size_t copied = s_pcm_reader(buffer, length, s_pcm_context);
    if (copied == 0) {
        return 0;
    }
    if (copied < length) {
        memset(buffer + copied, 0, length - copied);
    }
    return length;
}

static void audio_pump_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_state.audio_connected) {
            esp_hf_client_outgoing_data_ready();
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_PUMP_PERIOD_MS));
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool connect_hid = false;
        bool connect_hfp = false;
        esp_bd_addr_t address = {0};
        int64_t current_ms = now_ms();

        portENTER_CRITICAL(&s_reconnect_lock);
        if (s_reconnect_target_valid && !s_state.pairing &&
            current_ms - s_last_reconnect_request_ms >=
                RECONNECT_REQUEST_GAP_MS) {
            if (s_hid_connecting && current_ms >= s_hid_retry_at_ms) {
                s_hid_connecting = false;
                schedule_hid_retry(current_ms, false);
            }
            if (s_hfp_connecting && current_ms >= s_hfp_retry_at_ms) {
                s_hfp_connecting = false;
                schedule_hfp_retry(current_ms, false);
            }
            if (s_hid_ready && !s_state.hid_connected &&
                !s_hid_connecting && current_ms >= s_hid_retry_at_ms) {
                s_hid_connecting = true;
                s_hid_retry_at_ms =
                    current_ms + RECONNECT_ATTEMPT_TIMEOUT_MS;
                s_last_reconnect_request_ms = current_ms;
                connect_hid = true;
                memcpy(address, s_reconnect_address, sizeof(address));
            } else if (s_hfp_ready && !s_state.hfp_connected &&
                       !s_hfp_connecting &&
                       current_ms >= s_hfp_retry_at_ms) {
                s_hfp_connecting = true;
                s_hfp_retry_at_ms =
                    current_ms + RECONNECT_ATTEMPT_TIMEOUT_MS;
                s_last_reconnect_request_ms = current_ms;
                connect_hfp = true;
                memcpy(address, s_reconnect_address, sizeof(address));
            }
        }
        portEXIT_CRITICAL(&s_reconnect_lock);

        if (connect_hid) {
            esp_err_t err = esp_bt_hid_device_connect(address);
            ESP_LOGI(TAG, "automatic HID reconnect requested: %s",
                     esp_err_to_name(err));
            if (err != ESP_OK) {
                portENTER_CRITICAL(&s_reconnect_lock);
                s_hid_connecting = false;
                schedule_hid_retry(now_ms(), false);
                portEXIT_CRITICAL(&s_reconnect_lock);
            }
        } else if (connect_hfp) {
            esp_err_t err = esp_hf_client_connect(address);
            ESP_LOGI(TAG, "automatic HFP reconnect requested: %s",
                     esp_err_to_name(err));
            if (err != ESP_OK) {
                portENTER_CRITICAL(&s_reconnect_lock);
                s_hfp_connecting = false;
                schedule_hfp_retry(now_ms(), false);
                portEXIT_CRITICAL(&s_reconnect_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_TASK_PERIOD_MS));
    }
}

static bool load_first_bond(esp_bd_addr_t *address)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        return false;
    }
    int requested = 1;
    if (esp_bt_gap_get_bond_device_list(&requested, address) != ESP_OK ||
        requested <= 0) {
        return false;
    }
    return true;
}

esp_err_t vibe_bt_composite_init(vibe_bt_state_callback_t state_callback,
                                 void *context)
{
    s_state_callback = state_callback;
    s_state_context = context;

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "read BT mac");
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&controller_config), TAG,
                        "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG,
                        "controller enable");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");

    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(gap_callback), TAG,
                        "GAP callback");
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_device_name(DEVICE_NAME), TAG,
                        "device name");
    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_security_param(
                            ESP_BT_SP_IOCAP_MODE, &io_capability,
                            sizeof(io_capability)),
                        TAG, "IO capability");
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_pin(pin_type, 0, NULL), TAG, "PIN mode");

    esp_bt_cod_t cod = {
        .major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL,
        .minor = ESP_BT_COD_MINOR_PERIPHERAL_COMBO,
        .service = ESP_BT_COD_SRVC_CAPTURING | ESP_BT_COD_SRVC_AUDIO |
                   ESP_BT_COD_SRVC_TELEPHONY,
    };
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL), TAG,
                        "class of device");

    s_reconnect_target_valid = load_first_bond(&s_reconnect_address);
    ESP_RETURN_ON_ERROR(esp_hf_client_register_callback(hfp_callback), TAG,
                        "HFP callback");
    ESP_RETURN_ON_ERROR(esp_hf_client_init(), TAG, "HFP init");
    ESP_RETURN_ON_ERROR(esp_hf_client_register_data_callback(
                            hfp_incoming_audio, hfp_outgoing_audio),
                        TAG, "HFP audio callbacks");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BT,
                                          hid_callback, &s_hid),
                        TAG, "HID init");

    s_state.paired = esp_bt_gap_get_bond_device_num() > 0;
    set_scan_mode(false);
    if (xTaskCreatePinnedToCore(audio_pump_task, "bt_audio_pump", 2048, NULL,
                                6, &s_audio_pump_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(reconnect_task, "bt_reconnect", 3072, NULL,
                                5, &s_reconnect_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready name=%s serial=%s paired=%d free_heap=%" PRIu32,
             DEVICE_NAME, s_serial, s_state.paired, esp_get_free_heap_size());
    notify_state();
    return ESP_OK;
}

void vibe_bt_composite_set_pcm_reader(vibe_bt_pcm_read_fn reader,
                                      void *context)
{
    s_pcm_reader = reader;
    s_pcm_context = context;
}

esp_err_t vibe_bt_composite_begin_pairing(void)
{
    set_scan_mode(true);
    return ESP_OK;
}

esp_err_t vibe_bt_composite_end_pairing(void)
{
    set_scan_mode(false);
    return ESP_OK;
}

esp_err_t vibe_bt_composite_clear_bonds(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count > 0) {
        esp_bd_addr_t *addresses = calloc((size_t)count, sizeof(esp_bd_addr_t));
        ESP_RETURN_ON_FALSE(addresses != NULL, ESP_ERR_NO_MEM, TAG,
                            "bond list allocation");
        int requested = count;
        esp_err_t err = esp_bt_gap_get_bond_device_list(&requested, addresses);
        if (err == ESP_OK) {
            for (int i = 0; i < requested; ++i) {
                esp_err_t remove_err = esp_bt_gap_remove_bond_device(addresses[i]);
                if (remove_err != ESP_OK) {
                    err = remove_err;
                }
            }
        }
        free(addresses);
        ESP_RETURN_ON_ERROR(err, TAG, "remove bonds");
    }
    s_state.paired = false;
    s_state.hid_connected = false;
    s_state.hfp_connected = false;
    s_state.audio_connected = false;
    s_state.wideband = false;
    portENTER_CRITICAL(&s_reconnect_lock);
    s_reconnect_target_valid = false;
    s_hid_connecting = false;
    s_hfp_connecting = false;
    portEXIT_CRITICAL(&s_reconnect_lock);
    set_scan_mode(false);
    return ESP_OK;
}

esp_err_t vibe_bt_composite_request_reconnect(void)
{
    ESP_RETURN_ON_FALSE(s_state.paired, ESP_ERR_INVALID_STATE, TAG,
                        "no bonded host");
    int64_t current_ms = now_ms();
    portENTER_CRITICAL(&s_reconnect_lock);
    if (!s_reconnect_target_valid) {
        portEXIT_CRITICAL(&s_reconnect_lock);
        ESP_LOGE(TAG, "bonded host address unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    s_hid_connecting = false;
    s_hfp_connecting = false;
    s_hid_retry_at_ms = current_ms;
    s_hfp_retry_at_ms = current_ms;
    s_last_reconnect_request_ms =
        current_ms - RECONNECT_REQUEST_GAP_MS;
    portEXIT_CRITICAL(&s_reconnect_lock);
    ESP_LOGI(TAG, "automatic reconnect requested by user activity");
    return ESP_OK;
}

vibe_bt_composite_state_t vibe_bt_composite_state(void)
{
    return s_state;
}

static esp_err_t send_keyboard_report(uint8_t modifier, uint8_t keycode)
{
    ESP_RETURN_ON_FALSE(s_hid && esp_hidd_dev_connected(s_hid),
                        ESP_ERR_INVALID_STATE, TAG, "HID disconnected");
    uint8_t report[8] = {0};
    report[0] = modifier;
    report[2] = keycode;
    return esp_hidd_dev_input_set(s_hid, 0, HID_REPORT_ID_KEYBOARD,
                                  report, sizeof(report));
}

esp_err_t vibe_bt_composite_send_right_shift(bool pressed)
{
    return send_keyboard_report(pressed ? 0x20 : 0x00, 0x00);
}

esp_err_t vibe_bt_composite_send_enter(bool pressed)
{
    return send_keyboard_report(0x00, pressed ? 0x28 : 0x00);
}

esp_err_t vibe_bt_composite_send_mouse(int8_t dx, int8_t dy,
                                       bool left_pressed)
{
    ESP_RETURN_ON_FALSE(s_hid && esp_hidd_dev_connected(s_hid),
                        ESP_ERR_INVALID_STATE, TAG, "HID disconnected");
    uint8_t report[4] = {left_pressed ? 1 : 0, (uint8_t)dx, (uint8_t)dy, 0};
    return esp_hidd_dev_input_set(s_hid, 0, HID_REPORT_ID_MOUSE,
                                  report, sizeof(report));
}
