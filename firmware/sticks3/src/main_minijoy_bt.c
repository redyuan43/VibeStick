#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_bt_composite.h"
#include "vibe_bt_status_ui.h"
#include "vibe_input.h"
#include "vibe_minijoyc.h"
#include "vibe_minijoy_ota.h"
#include "vibe_stick_config.h"

#define APP_LOOP_MS 20
#define PAIRING_WINDOW_MS 120000
#define SIDE_CLEAR_HOLD_MS 5000
#define JOY_RETRY_MS 1000
#define JOY_DEADZONE 82
#define JOY_MAX_STEP 24
#define PCM_STAGING_BYTES 2048
#define PAIRING_LED_INTERVAL_MS 150
#define JOYSTICK_LED_HOLD_MS 200
#define MINIJOY_LED_OFF 0x000000
#define MINIJOY_LED_PAIRING 0x0000ff
#define MINIJOY_LED_MICROPHONE 0x400000
#define MINIJOY_LED_JOYSTICK 0x004000
#define STARTUP_PAIRING_DELAY_MS 1000
#define CONFIRM_WINDOW_MS 5000
#define STARTUP_OTA_HOLD_MS 600
#define STARTUP_OTA_POLL_MS 40
#define OTA_RESULT_DISPLAY_MS 2500
#define OTA_TASK_STACK_BYTES 8192

typedef enum {
    APP_EVENT_PTT_DOWN,
    APP_EVENT_PTT_UP,
    APP_EVENT_CLEAR_BONDS,
    APP_EVENT_WAKE_DISPLAY,
} app_event_t;

static const char *TAG = "minijoy_bt";
static QueueHandle_t s_event_queue;
static atomic_bool s_ptt_active;
static atomic_bool s_side_long_handled;
static portMUX_TYPE s_bt_state_lock = portMUX_INITIALIZER_UNLOCKED;
static vibe_bt_composite_state_t s_bt_state;
static uint8_t s_pcm_staging[PCM_STAGING_BYTES];
static size_t s_pcm_staging_offset;
static size_t s_pcm_staging_length;
static uint8_t s_resample_staging[PCM_STAGING_BYTES];
static bool s_minijoy_ready;
static bool s_minijoy_button_down;
static int64_t s_minijoy_retry_ms;
static int64_t s_pairing_deadline_ms;
static int64_t s_startup_pairing_due_ms;
static int64_t s_pairing_led_toggle_ms;
static int64_t s_joystick_led_until_ms;
static uint32_t s_minijoy_led_color = UINT32_MAX;
static bool s_pairing_led_on;
static bool s_confirm_key_down;
static int64_t s_confirm_deadline_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void queue_app_event(app_event_t event)
{
    if (s_event_queue) {
        xQueueSend(s_event_queue, &event, 0);
    }
}

static void front_down_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    queue_app_event(APP_EVENT_PTT_DOWN);
}

static void front_up_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    queue_app_event(APP_EVENT_PTT_UP);
}

static void side_long_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    atomic_store(&s_side_long_handled, true);
    queue_app_event(APP_EVENT_CLEAR_BONDS);
}

static void side_up_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    if (atomic_exchange(&s_side_long_handled, false)) {
        return;
    }
    queue_app_event(APP_EVENT_WAKE_DISPLAY);
}

static void bt_state_callback(const vibe_bt_composite_state_t *state,
                              void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_bt_state_lock);
    s_bt_state = *state;
    portEXIT_CRITICAL(&s_bt_state_lock);
}

static vibe_bt_composite_state_t bt_state(void)
{
    vibe_bt_composite_state_t state;
    portENTER_CRITICAL(&s_bt_state_lock);
    state = s_bt_state;
    portEXIT_CRITICAL(&s_bt_state_lock);
    return state;
}

static void clear_confirm_window(void)
{
    if (s_confirm_key_down) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_send_enter(false));
        s_confirm_key_down = false;
    }
    s_confirm_deadline_ms = 0;
    vibe_bt_status_ui_set_confirm_window(false);
}

static size_t read_raw_pcm(uint8_t *buffer, size_t length)
{
    size_t copied = 0;
    while (copied < length) {
        if (s_pcm_staging_offset >= s_pcm_staging_length) {
            size_t chunk_length = 0;
            if (vibe_audio_read(s_pcm_staging, sizeof(s_pcm_staging),
                                &chunk_length, 0) != ESP_OK) {
                break;
            }
            s_pcm_staging_offset = 0;
            s_pcm_staging_length = chunk_length;
        }
        size_t available = s_pcm_staging_length - s_pcm_staging_offset;
        size_t requested = length - copied;
        size_t part = available < requested ? available : requested;
        memcpy(buffer + copied, s_pcm_staging + s_pcm_staging_offset, part);
        s_pcm_staging_offset += part;
        copied += part;
    }
    return copied;
}

static size_t read_hfp_pcm(uint8_t *buffer, size_t length, void *context)
{
    (void)context;
    if (!atomic_load(&s_ptt_active)) {
        return 0;
    }
    vibe_bt_composite_state_t state = bt_state();
    if (state.wideband) {
        return read_raw_pcm(buffer, length);
    }

    if (length * 2 > sizeof(s_resample_staging) || (length & 1u)) {
        return 0;
    }
    size_t source_length = read_raw_pcm(s_resample_staging, length * 2);
    if (source_length < length * 2) {
        return 0;
    }
    int16_t *source = (int16_t *)s_resample_staging;
    int16_t *target = (int16_t *)buffer;
    size_t samples = length / sizeof(int16_t);
    for (size_t i = 0; i < samples; ++i) {
        target[i] = (int16_t)(((int32_t)source[i * 2] +
                               (int32_t)source[i * 2 + 1]) /
                              2);
    }
    return length;
}

static int8_t joystick_axis(int16_t value)
{
    int magnitude = value < 0 ? -value : value;
    if (magnitude <= JOY_DEADZONE) {
        return 0;
    }
    if (magnitude > 511) {
        magnitude = 511;
    }
    int linear = (magnitude - JOY_DEADZONE) * JOY_MAX_STEP /
                 (511 - JOY_DEADZONE);
    int curved = linear * linear / JOY_MAX_STEP;
    if (curved < 1) {
        curved = 1;
    } else if (curved > JOY_MAX_STEP) {
        curved = JOY_MAX_STEP;
    }
    return (int8_t)(value < 0 ? -curved : curved);
}

static void close_minijoy(void)
{
    if (s_minijoy_ready) {
        vibe_bt_composite_send_mouse(0, 0, false);
        vibe_minijoyc_close();
        s_minijoy_ready = false;
        s_minijoy_button_down = false;
        s_minijoy_led_color = UINT32_MAX;
        s_pairing_led_toggle_ms = 0;
    }
}

static void set_minijoy_led(uint32_t color)
{
    if (!s_minijoy_ready || color == s_minijoy_led_color) {
        return;
    }
    if (vibe_minijoyc_set_led(color) == ESP_OK) {
        s_minijoy_led_color = color;
    }
}

static void update_minijoy_led(int64_t current_ms)
{
    if (!s_minijoy_ready) {
        return;
    }
    vibe_bt_composite_state_t state = bt_state();
    if (state.pairing) {
        if (s_pairing_led_toggle_ms == 0 ||
            current_ms >= s_pairing_led_toggle_ms) {
            s_pairing_led_on = !s_pairing_led_on;
            s_pairing_led_toggle_ms =
                current_ms + PAIRING_LED_INTERVAL_MS;
            set_minijoy_led(s_pairing_led_on ? MINIJOY_LED_PAIRING
                                             : MINIJOY_LED_OFF);
        }
        return;
    }
    s_pairing_led_toggle_ms = 0;
    s_pairing_led_on = false;
    set_minijoy_led(current_ms < s_joystick_led_until_ms
                        ? MINIJOY_LED_JOYSTICK
                        : MINIJOY_LED_OFF);
}

static void open_minijoy(void)
{
    if (atomic_load(&s_ptt_active) || s_minijoy_ready ||
        now_ms() < s_minijoy_retry_ms) {
        return;
    }
    esp_err_t err = vibe_minijoyc_open();
    if (err == ESP_OK) {
        s_minijoy_ready = true;
        s_minijoy_led_color = UINT32_MAX;
        set_minijoy_led(MINIJOY_LED_OFF);
        ESP_LOGI(TAG, "MiniJoy ready on GPIO0/GPIO26");
    } else {
        s_minijoy_retry_ms = now_ms() + JOY_RETRY_MS;
        ESP_LOGW(TAG, "MiniJoy open failed: %s", esp_err_to_name(err));
    }
}

static void start_ptt(void)
{
    if (atomic_load(&s_ptt_active)) {
        return;
    }
    clear_confirm_window();
    set_minijoy_led(MINIJOY_LED_MICROPHONE);
    s_joystick_led_until_ms = 0;
    close_minijoy();
    esp_err_t err = vibe_minijoyc_suspend_for_microphone();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MiniJoy suspend failed: %s", esp_err_to_name(err));
        s_minijoy_retry_ms = now_ms() + 20;
        open_minijoy();
        vibe_bt_status_ui_set(VIBE_BT_UI_ERROR, s_minijoy_ready);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    s_pcm_staging_offset = 0;
    s_pcm_staging_length = 0;
    err = vibe_audio_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microphone start failed: %s", esp_err_to_name(err));
        s_minijoy_retry_ms = now_ms() + 20;
        open_minijoy();
        vibe_bt_status_ui_set(VIBE_BT_UI_ERROR, s_minijoy_ready);
        return;
    }
    atomic_store(&s_ptt_active, true);
    err = vibe_bt_composite_send_right_shift(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Right Shift down failed: %s", esp_err_to_name(err));
    }
    vibe_bt_status_ui_set(VIBE_BT_UI_RECORDING, false);
    ESP_LOGI(TAG,
             "PTT started; MiniJoy paused free_heap=%u min_free_heap=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void stop_ptt(void)
{
    if (!atomic_load(&s_ptt_active)) {
        return;
    }
    esp_err_t err = vibe_bt_composite_send_right_shift(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Right Shift up failed: %s", esp_err_to_name(err));
    }
    atomic_store(&s_ptt_active, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_audio_stop());
    s_pcm_staging_offset = 0;
    s_pcm_staging_length = 0;
    s_minijoy_retry_ms = now_ms() + 20;
    open_minijoy();
    s_confirm_deadline_ms = now_ms() + CONFIRM_WINDOW_MS;
    vibe_bt_status_ui_set_confirm_window(true);
    ESP_LOGI(TAG,
             "PTT stopped; MiniJoy resumed=%d confirm_window_ms=%d free_heap=%u min_free_heap=%u",
             s_minijoy_ready, CONFIRM_WINDOW_MS,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void handle_event(app_event_t event)
{
    switch (event) {
    case APP_EVENT_PTT_DOWN:
        start_ptt();
        break;
    case APP_EVENT_PTT_UP:
        stop_ptt();
        break;
    case APP_EVENT_CLEAR_BONDS:
        stop_ptt();
        clear_confirm_window();
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_clear_bonds());
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_begin_pairing());
        s_pairing_deadline_ms = now_ms() + PAIRING_WINDOW_MS;
        s_startup_pairing_due_ms = 0;
        s_pairing_led_toggle_ms = 0;
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, s_minijoy_ready);
        ESP_LOGI(TAG, "Bluetooth bonds cleared; pairing window started");
        break;
    case APP_EVENT_WAKE_DISPLAY:
        vibe_bt_status_ui_activity();
        break;
    }
}

static void poll_minijoy(void)
{
    if (atomic_load(&s_ptt_active)) {
        return;
    }
    open_minijoy();
    if (!s_minijoy_ready) {
        return;
    }
    vibe_minijoyc_state_t joy = {0};
    esp_err_t err = vibe_minijoyc_read(&joy);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MiniJoy read failed: %s", esp_err_to_name(err));
        set_minijoy_led(MINIJOY_LED_OFF);
        close_minijoy();
        s_minijoy_retry_ms = now_ms() + JOY_RETRY_MS;
        return;
    }

    vibe_bt_composite_state_t state = bt_state();
    bool button_changed = joy.button_pressed != s_minijoy_button_down;
    if (button_changed && joy.button_pressed && !state.hid_connected) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_begin_pairing());
        s_pairing_deadline_ms = now_ms() + PAIRING_WINDOW_MS;
        s_startup_pairing_due_ms = 0;
        s_pairing_led_toggle_ms = 0;
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, true);
        vibe_bt_status_ui_activity();
    } else if (state.hid_connected) {
        int8_t dx = joystick_axis(joy.x);
        int8_t dy = (int8_t)-joystick_axis(joy.y);
        if (button_changed && joy.button_pressed &&
            s_confirm_deadline_ms > now_ms()) {
            esp_err_t enter_err = vibe_bt_composite_send_enter(true);
            if (enter_err == ESP_OK) {
                s_confirm_key_down = true;
                s_confirm_deadline_ms = 0;
                vibe_bt_status_ui_set_confirm_window(false);
                ESP_LOGI(TAG, "confirm window sent Enter");
            } else {
                ESP_LOGW(TAG, "Enter down failed: %s",
                         esp_err_to_name(enter_err));
            }
        } else if (button_changed && !joy.button_pressed &&
                   s_confirm_key_down) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_send_enter(false));
            s_confirm_key_down = false;
        }
        if (dx != 0 || dy != 0 || button_changed) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_send_mouse(
                dx, dy, s_confirm_key_down ? false : joy.button_pressed));
            vibe_bt_status_ui_activity();
        }
        if (dx != 0 || dy != 0 || joy.button_pressed || button_changed) {
            s_joystick_led_until_ms = now_ms() + JOYSTICK_LED_HOLD_MS;
        }
    }
    s_minijoy_button_down = joy.button_pressed;
}

static void update_status(void)
{
    vibe_bt_composite_state_t state = bt_state();
    if (state.pairing && (state.hid_connected || state.hfp_connected)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_end_pairing());
        s_pairing_deadline_ms = 0;
        state = bt_state();
    }
    if (atomic_load(&s_ptt_active)) {
        vibe_bt_status_ui_set(VIBE_BT_UI_RECORDING, false);
    } else if (state.pairing) {
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, s_minijoy_ready);
    } else if (state.hid_connected || state.hfp_connected) {
        vibe_bt_status_ui_set(VIBE_BT_UI_CONNECTED, s_minijoy_ready);
    } else {
        vibe_bt_status_ui_set(VIBE_BT_UI_WAITING, s_minijoy_ready);
    }

}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

static bool startup_ota_requested(void)
{
    if (vibe_minijoyc_open() != ESP_OK) {
        ESP_LOGW(TAG, "MiniJoy unavailable for OTA startup gesture");
        return false;
    }
    bool held = true;
    int samples = STARTUP_OTA_HOLD_MS / STARTUP_OTA_POLL_MS;
    for (int sample = 0; sample < samples; ++sample) {
        vibe_minijoyc_state_t state = {0};
        if (vibe_minijoyc_read(&state) != ESP_OK ||
            !state.button_pressed) {
            held = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(STARTUP_OTA_POLL_MS));
    }
    if (held) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_minijoyc_set_led(0xffa000));
    }
    vibe_minijoyc_close();
    return held;
}

static void ota_status_callback(vibe_minijoy_ota_status_t status,
                                void *context)
{
    (void)context;
    vibe_bt_ui_status_t ui_status = VIBE_BT_UI_ERROR;
    switch (status) {
    case VIBE_MINIJOY_OTA_CONNECTING:
        ui_status = VIBE_BT_UI_OTA_CONNECTING;
        break;
    case VIBE_MINIJOY_OTA_CHECKING:
        ui_status = VIBE_BT_UI_OTA_CHECKING;
        break;
    case VIBE_MINIJOY_OTA_DOWNLOADING:
        ui_status = VIBE_BT_UI_OTA_DOWNLOADING;
        break;
    case VIBE_MINIJOY_OTA_CURRENT:
        ui_status = VIBE_BT_UI_OTA_CURRENT;
        break;
    case VIBE_MINIJOY_OTA_FAILED:
        ui_status = VIBE_BT_UI_OTA_FAILED;
        break;
    }
    vibe_bt_status_ui_set(ui_status, false);
}

static void ota_maintenance_task(void *context)
{
    (void)context;
    esp_err_t ota_err = vibe_minijoy_ota_run(ota_status_callback, NULL);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA maintenance failed: %s",
                 esp_err_to_name(ota_err));
    }
    vTaskDelay(pdMS_TO_TICKS(OTA_RESULT_DISPLAY_MS));
    esp_restart();
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(vibe_board_init_power());
    ESP_ERROR_CHECK(vibe_board_set_external_5v(false));
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_ERROR_CHECK(vibe_board_set_external_5v(true));
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_ERROR_CHECK(vibe_bt_status_ui_init());
    if (startup_ota_requested()) {
        ESP_LOGI(TAG, "startup gesture selected OTA maintenance mode");
        BaseType_t created = xTaskCreatePinnedToCore(
            ota_maintenance_task, "minijoy_ota", OTA_TASK_STACK_BYTES, NULL,
            4, NULL, 0);
        ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        return;
    }
    ESP_ERROR_CHECK(vibe_audio_init());

    s_event_queue = xQueueCreate(12, sizeof(app_event_t));
    ESP_ERROR_CHECK(s_event_queue ? ESP_OK : ESP_ERR_NO_MEM);
    vibe_bt_composite_set_pcm_reader(read_hfp_pcm, NULL);
    ESP_ERROR_CHECK(vibe_bt_composite_init(bt_state_callback, NULL));

    const vibe_input_config_t input_config = {
        .front_long_ms = 60000,
        .front_confirm_ms = 60000,
        .side_mode_ms = SIDE_CLEAR_HOLD_MS,
        .side_calibration_ms = 60000,
    };
    const vibe_input_callbacks_t input_callbacks = {
        .front_down = front_down_callback,
        .front_up = front_up_callback,
        .side_up = side_up_callback,
        .side_mode_hold = side_long_callback,
    };
    ESP_ERROR_CHECK(vibe_input_init(&input_config, &input_callbacks));
    open_minijoy();
    vibe_bt_composite_state_t initial_state = bt_state();
    if (!initial_state.hid_connected && !initial_state.hfp_connected) {
        s_startup_pairing_due_ms = now_ms() + STARTUP_PAIRING_DELAY_MS;
        ESP_LOGI(TAG, "startup pairing window scheduled");
    }
    update_status();

    ESP_LOGI(TAG, "dedicated MiniJoy BT firmware ready version=%s",
             FIRMWARE_VERSION);
    for (;;) {
        app_event_t event;
        while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
            handle_event(event);
        }
        poll_minijoy();
        int64_t current_ms = now_ms();
        if (s_confirm_deadline_ms > 0 &&
            current_ms >= s_confirm_deadline_ms) {
            s_confirm_deadline_ms = 0;
            vibe_bt_status_ui_set_confirm_window(false);
            ESP_LOGI(TAG, "confirm window expired");
        }
        if (s_startup_pairing_due_ms > 0 &&
            current_ms >= s_startup_pairing_due_ms) {
            s_startup_pairing_due_ms = 0;
            ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_begin_pairing());
            s_pairing_deadline_ms = current_ms + PAIRING_WINDOW_MS;
            s_pairing_led_toggle_ms = 0;
            ESP_LOGI(TAG, "startup pairing window started");
        }
        if (s_pairing_deadline_ms > 0 && current_ms >= s_pairing_deadline_ms) {
            vibe_bt_composite_end_pairing();
            s_pairing_deadline_ms = 0;
        }
        update_status();
        update_minijoy_led(current_ms);
        vibe_bt_status_ui_tick(current_ms, vibe_audio_level_percent());
        vTaskDelay(pdMS_TO_TICKS(APP_LOOP_MS));
    }
}
