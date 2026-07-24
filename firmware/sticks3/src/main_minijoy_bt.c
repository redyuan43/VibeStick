#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "vibe_air_mouse.h"
#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_bt_composite.h"
#include "vibe_bt_status_ui.h"
#include "vibe_input.h"
#include "vibe_minijoyc.h"
#include "vibe_minijoy_ota.h"
#include "vibe_motion.h"
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
#define PTT_RELEASE_AUDIO_TAIL_MS 350
#define STARTUP_OTA_HOLD_MS 600
#define STARTUP_OTA_POLL_MS 40
#define OTA_RESULT_DISPLAY_MS 2500
#define OTA_TASK_STACK_BYTES 8192
#define DEEP_SLEEP_IDLE_MS 600000
#define DEEP_SLEEP_RETRY_MS 5000
#define WAKE_RELEASE_STABLE_MS 80
#define IMU_READ_ERROR_LIMIT 5
#define AIR_MOUSE_CALIBRATION_LOG_MS 1000

typedef enum {
    APP_EVENT_FRONT_DOWN,
    APP_EVENT_FRONT_UP,
    APP_EVENT_HFP_AUDIO_CONNECTED,
    APP_EVENT_HFP_AUDIO_DISCONNECTED,
    APP_EVENT_TOGGLE_AIR_MOUSE,
    APP_EVENT_CLEAR_BONDS,
} app_event_t;

typedef enum {
    CAPTURE_OWNER_NONE,
    CAPTURE_OWNER_DEVICE_PTT,
    CAPTURE_OWNER_HOST_HFP,
} capture_owner_t;

static const char *TAG = "minijoy_bt";
static QueueHandle_t s_event_queue;
static atomic_bool s_capture_active;
static atomic_bool s_side_long_handled;
static atomic_bool s_wake_input_guard;
static portMUX_TYPE s_bt_state_lock = portMUX_INITIALIZER_UNLOCKED;
static vibe_bt_composite_state_t s_bt_state;
static uint8_t s_pcm_staging[PCM_STAGING_BYTES];
static size_t s_pcm_staging_offset;
static size_t s_pcm_staging_length;
static uint8_t s_resample_staging[PCM_STAGING_BYTES];
static bool s_minijoy_ready;
static bool s_minijoy_button_down;
static bool s_joystick_motion_active;
static int64_t s_minijoy_retry_ms;
static int64_t s_pairing_deadline_ms;
static int64_t s_startup_pairing_due_ms;
static int64_t s_pairing_led_toggle_ms;
static int64_t s_joystick_led_until_ms;
static uint32_t s_minijoy_led_color = UINT32_MAX;
static bool s_pairing_led_on;
static bool s_confirm_button_consumed;
static bool s_front_confirm_consumed;
static vibe_air_mouse_t s_air_mouse;
static bool s_air_mouse_enabled;
static bool s_air_mouse_calibrated;
static bool s_air_mouse_left_down;
static bool s_air_mouse_ignore_front_until_up;
static bool s_imu_available;
static bool s_imu_error;
static unsigned int s_imu_read_errors;
static int64_t s_last_imu_sample_ms;
static int64_t s_last_calibration_log_ms;
static int64_t s_confirm_deadline_ms;
static int64_t s_last_activity_ms;
static int64_t s_next_deep_sleep_attempt_ms;
static int64_t s_wake_release_since_ms;
static capture_owner_t s_capture_owner;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void register_activity(void)
{
    s_last_activity_ms = now_ms();
    s_next_deep_sleep_attempt_ms = 0;
    vibe_bt_status_ui_activity();
}

static void play_event_sound(agent_sound_t sound, const char *event)
{
    esp_err_t err = vibe_audio_play_sound(sound);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s sound failed: %s", event, esp_err_to_name(err));
    }
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
    if (!atomic_load(&s_wake_input_guard)) {
        queue_app_event(APP_EVENT_FRONT_DOWN);
    }
}

static void front_up_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    if (!atomic_load(&s_wake_input_guard)) {
        queue_app_event(APP_EVENT_FRONT_UP);
    }
}

static void side_long_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    if (atomic_load(&s_wake_input_guard)) {
        return;
    }
    atomic_store(&s_side_long_handled, true);
    queue_app_event(APP_EVENT_CLEAR_BONDS);
}

static void side_up_callback(void *button, void *context)
{
    (void)button;
    (void)context;
    if (atomic_load(&s_wake_input_guard)) {
        atomic_store(&s_side_long_handled, false);
        return;
    }
    if (atomic_exchange(&s_side_long_handled, false)) {
        return;
    }
    queue_app_event(APP_EVENT_TOGGLE_AIR_MOUSE);
}

static void bt_state_callback(const vibe_bt_composite_state_t *state,
                              void *context)
{
    (void)context;
    bool audio_was_connected;
    portENTER_CRITICAL(&s_bt_state_lock);
    audio_was_connected = s_bt_state.audio_connected;
    s_bt_state = *state;
    portEXIT_CRITICAL(&s_bt_state_lock);
    if (state->audio_connected != audio_was_connected) {
        queue_app_event(state->audio_connected
                            ? APP_EVENT_HFP_AUDIO_CONNECTED
                            : APP_EVENT_HFP_AUDIO_DISCONNECTED);
    }
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
    s_confirm_button_consumed = false;
    s_front_confirm_consumed = false;
    s_confirm_deadline_ms = 0;
    vibe_bt_status_ui_set_confirm_window(false);
}

static bool request_hid_connection(void)
{
    vibe_bt_composite_state_t state = bt_state();
    if (state.hid_connected) {
        return true;
    }
    if (state.paired) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_bt_composite_request_reconnect());
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_begin_pairing());
        s_pairing_deadline_ms = now_ms() + PAIRING_WINDOW_MS;
        s_startup_pairing_due_ms = 0;
        s_pairing_led_toggle_ms = 0;
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, s_minijoy_ready);
    }
    play_event_sound(VIBE_STICK_SOUND_PAIRING, "pairing request");
    register_activity();
    return false;
}

static bool consume_confirm_window(const char *source)
{
    if (s_confirm_deadline_ms <= now_ms()) {
        return false;
    }
    s_confirm_deadline_ms = 0;
    vibe_bt_status_ui_set_confirm_window(false);
    esp_err_t err = vibe_bt_composite_send_enter_click();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s sent HID Enter click", source);
        play_event_sound(VIBE_STICK_SOUND_FOLLOWUP_ENTER,
                         "follow-up Enter");
    } else {
        ESP_LOGW(TAG, "%s HID Enter click failed: %s", source,
                 esp_err_to_name(err));
        play_event_sound(VIBE_STICK_SOUND_ERROR, "follow-up Enter error");
    }
    return true;
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
    if (!atomic_load(&s_capture_active)) {
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
        s_joystick_motion_active = false;
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
    if (atomic_load(&s_capture_active) || s_minijoy_ready ||
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
    if (atomic_load(&s_capture_active)) {
        return;
    }
    clear_confirm_window();
    play_event_sound(VIBE_STICK_SOUND_RECORDING_START, "recording start");
    set_minijoy_led(MINIJOY_LED_MICROPHONE);
    s_joystick_led_until_ms = 0;
    close_minijoy();
    esp_err_t err = vibe_minijoyc_suspend_for_microphone();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MiniJoy suspend failed: %s", esp_err_to_name(err));
        s_minijoy_retry_ms = now_ms() + 20;
        open_minijoy();
        vibe_bt_status_ui_set(VIBE_BT_UI_ERROR, s_minijoy_ready);
        play_event_sound(VIBE_STICK_SOUND_ERROR, "MiniJoy suspend error");
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
        play_event_sound(VIBE_STICK_SOUND_ERROR, "microphone start error");
        return;
    }
    s_capture_owner = CAPTURE_OWNER_DEVICE_PTT;
    atomic_store(&s_capture_active, true);
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
    if (s_capture_owner != CAPTURE_OWNER_DEVICE_PTT) {
        return;
    }
    esp_err_t err = vibe_bt_composite_send_right_shift(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Right Shift up failed: %s", esp_err_to_name(err));
    }
    /* CapsWriter finalizes 300 ms after key-up; keep SCO PCM alive through it. */
    vTaskDelay(pdMS_TO_TICKS(PTT_RELEASE_AUDIO_TAIL_MS));
    atomic_store(&s_capture_active, false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_audio_stop());
    s_capture_owner = CAPTURE_OWNER_NONE;
    play_event_sound(VIBE_STICK_SOUND_RECORDING_STOP, "recording stop");
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

static void start_host_capture(void)
{
    if (atomic_load(&s_capture_active)) {
        return;
    }
    clear_confirm_window();
    play_event_sound(VIBE_STICK_SOUND_RECORDING_START,
                     "host recording start");
    set_minijoy_led(MINIJOY_LED_MICROPHONE);
    s_joystick_led_until_ms = 0;
    close_minijoy();
    esp_err_t err = vibe_minijoyc_suspend_for_microphone();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MiniJoy suspend for host capture failed: %s",
                 esp_err_to_name(err));
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
        ESP_LOGE(TAG, "host microphone capture start failed: %s",
                 esp_err_to_name(err));
        s_minijoy_retry_ms = now_ms() + 20;
        open_minijoy();
        vibe_bt_status_ui_set(VIBE_BT_UI_ERROR, s_minijoy_ready);
        return;
    }
    s_capture_owner = CAPTURE_OWNER_HOST_HFP;
    atomic_store(&s_capture_active, true);
    vibe_bt_status_ui_set(VIBE_BT_UI_RECORDING, false);
    register_activity();
    ESP_LOGI(TAG, "host-initiated HFP capture started");
}

static void stop_host_capture(void)
{
    if (s_capture_owner != CAPTURE_OWNER_HOST_HFP) {
        return;
    }
    atomic_store(&s_capture_active, false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_audio_stop());
    s_capture_owner = CAPTURE_OWNER_NONE;
    play_event_sound(VIBE_STICK_SOUND_RECORDING_STOP,
                     "host recording stop");
    s_pcm_staging_offset = 0;
    s_pcm_staging_length = 0;
    s_minijoy_retry_ms = now_ms() + 20;
    open_minijoy();
    register_activity();
    ESP_LOGI(TAG,
             "host-initiated HFP capture stopped; MiniJoy resumed=%d",
             s_minijoy_ready);
}

static void release_air_mouse_button(void)
{
    if (!s_air_mouse_left_down) {
        return;
    }
    esp_err_t err = vibe_bt_composite_send_mouse(0, 0, false);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "air mouse left button up failed: %s",
                 esp_err_to_name(err));
    }
    s_air_mouse_left_down = false;
}

static bool ensure_imu_ready(void)
{
    if (!s_imu_available) {
        esp_err_t err = vibe_motion_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MPU6886 init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_imu_available = true;
    }
    if (vibe_motion_suspended()) {
        esp_err_t err = vibe_motion_resume();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MPU6886 resume failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

static void enter_air_mouse_mode(void)
{
    stop_ptt();
    clear_confirm_window();
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_bt_composite_send_mouse(0, 0, false));
    s_confirm_button_consumed = false;
    s_front_confirm_consumed = false;
    s_air_mouse_ignore_front_until_up = vibe_input_front_pressed();

    if (!ensure_imu_ready()) {
        s_imu_error = true;
        play_event_sound(VIBE_STICK_SOUND_ERROR, "air mouse IMU error");
        register_activity();
        return;
    }

    vibe_air_mouse_reset_motion(&s_air_mouse);
    s_air_mouse_enabled = true;
    s_air_mouse_calibrated = vibe_air_mouse_calibrated(&s_air_mouse);
    s_air_mouse_left_down = false;
    s_imu_error = false;
    s_imu_read_errors = 0;
    s_last_imu_sample_ms = 0;
    s_last_calibration_log_ms = 0;
    vibe_bt_status_ui_set_air_mouse(true, s_air_mouse_calibrated);
    play_event_sound(VIBE_STICK_SOUND_SIDE_BUTTON, "air mouse enabled");
    register_activity();
    (void)request_hid_connection();
    if (s_air_mouse_calibrated) {
        ESP_LOGI(TAG, "mode=AIR_MOUSE_ACTIVE calibration reused");
    } else {
        ESP_LOGI(TAG, "mode=AIR_MOUSE_CALIBRATING keep device still");
    }
    ESP_LOGI(TAG,
             "air mouse mapping cursor_x=gyro_z cursor_y=gyro_x "
             "forward=WheelUp backward=WheelDown");
}

static void exit_air_mouse_mode(void)
{
    release_air_mouse_button();
    s_air_mouse_enabled = false;
    s_air_mouse_calibrated = false;
    s_air_mouse_ignore_front_until_up = vibe_input_front_pressed();
    s_imu_error = false;
    s_imu_read_errors = 0;
    s_last_imu_sample_ms = 0;
    vibe_bt_status_ui_set_air_mouse(false, false);
    if (s_imu_available && !vibe_motion_suspended()) {
        esp_err_t err = vibe_motion_suspend();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MPU6886 suspend failed: %s",
                     esp_err_to_name(err));
        }
    }
    play_event_sound(VIBE_STICK_SOUND_SIDE_BUTTON, "air mouse disabled");
    register_activity();
    ESP_LOGI(TAG, "mode=JOYSTICK_PTT");
}

static void handle_front_down(void)
{
    register_activity();
    if (s_air_mouse_ignore_front_until_up) {
        return;
    }
    if (!s_air_mouse_enabled) {
        if (!atomic_load(&s_capture_active) &&
            consume_confirm_window("front follow-up")) {
            s_front_confirm_consumed = true;
        } else {
            start_ptt();
        }
        return;
    }
    if (!request_hid_connection()) {
        return;
    }
    esp_err_t err = vibe_bt_composite_send_mouse(0, 0, true);
    if (err == ESP_OK) {
        s_air_mouse_left_down = true;
        play_event_sound(VIBE_STICK_SOUND_MOUSE_CLICK,
                         "air mouse click");
    } else {
        ESP_LOGW(TAG, "air mouse left button down failed: %s",
                 esp_err_to_name(err));
    }
}

static void handle_front_up(void)
{
    register_activity();
    if (s_air_mouse_ignore_front_until_up) {
        s_air_mouse_ignore_front_until_up = false;
        return;
    }
    if (s_air_mouse_enabled) {
        release_air_mouse_button();
    } else if (s_front_confirm_consumed) {
        s_front_confirm_consumed = false;
    } else {
        stop_ptt();
    }
}

static void handle_event(app_event_t event)
{
    switch (event) {
    case APP_EVENT_FRONT_DOWN:
        handle_front_down();
        break;
    case APP_EVENT_FRONT_UP:
        handle_front_up();
        break;
    case APP_EVENT_HFP_AUDIO_CONNECTED:
        start_host_capture();
        break;
    case APP_EVENT_HFP_AUDIO_DISCONNECTED:
        stop_host_capture();
        break;
    case APP_EVENT_TOGGLE_AIR_MOUSE:
        if (s_air_mouse_enabled) {
            exit_air_mouse_mode();
        } else {
            enter_air_mouse_mode();
        }
        break;
    case APP_EVENT_CLEAR_BONDS:
        register_activity();
        stop_ptt();
        release_air_mouse_button();
        clear_confirm_window();
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_clear_bonds());
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_begin_pairing());
        s_pairing_deadline_ms = now_ms() + PAIRING_WINDOW_MS;
        s_startup_pairing_due_ms = 0;
        s_pairing_led_toggle_ms = 0;
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, s_minijoy_ready);
        play_event_sound(VIBE_STICK_SOUND_PAIRING, "pairing");
        ESP_LOGI(TAG, "Bluetooth bonds cleared; pairing window started");
        break;
    }
}

static void poll_minijoy(void)
{
    if (atomic_load(&s_capture_active)) {
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
    if (s_air_mouse_enabled) {
        s_minijoy_button_down = joy.button_pressed;
        s_joystick_motion_active = false;
        return;
    }
    if (button_changed && joy.button_pressed && !state.hid_connected) {
        (void)request_hid_connection();
        s_joystick_motion_active = false;
    } else if (state.hid_connected) {
        int8_t dx = joystick_axis(joy.x);
        int8_t dy = (int8_t)-joystick_axis(joy.y);
        bool motion_active = dx != 0 || dy != 0;
        bool motion_started = motion_active && !s_joystick_motion_active;
        bool mouse_click = false;
        if (button_changed && joy.button_pressed &&
            consume_confirm_window("MiniJoy follow-up")) {
            s_confirm_button_consumed = true;
        } else if (button_changed && joy.button_pressed) {
            mouse_click = true;
        } else if (button_changed && !joy.button_pressed &&
                   s_confirm_button_consumed) {
            s_confirm_button_consumed = false;
        }
        if (dx != 0 || dy != 0 || button_changed) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_send_mouse(
                dx, dy,
                s_confirm_button_consumed ? false : joy.button_pressed));
            register_activity();
        }
        if (mouse_click) {
            play_event_sound(VIBE_STICK_SOUND_MOUSE_CLICK, "mouse click");
        } else if (motion_started && !button_changed) {
            play_event_sound(VIBE_STICK_SOUND_JOYSTICK_START,
                             "joystick start");
        }
        s_joystick_motion_active = motion_active;
        if (dx != 0 || dy != 0 || joy.button_pressed || button_changed) {
            s_joystick_led_until_ms = now_ms() + JOYSTICK_LED_HOLD_MS;
        }
    } else {
        s_joystick_motion_active = false;
    }
    s_minijoy_button_down = joy.button_pressed;
}

static void poll_air_mouse(int64_t current_ms)
{
    if (!s_air_mouse_enabled) {
        return;
    }

    vibe_motion_sample_t motion_sample = {0};
    esp_err_t err = vibe_motion_read_raw_sample(&motion_sample);
    if (err != ESP_OK) {
        ++s_imu_read_errors;
        if (s_imu_read_errors <= IMU_READ_ERROR_LIMIT) {
            ESP_LOGW(TAG, "MPU6886 read failed count=%u: %s",
                     s_imu_read_errors, esp_err_to_name(err));
        }
        if (s_imu_read_errors == IMU_READ_ERROR_LIMIT) {
            s_imu_error = true;
            release_air_mouse_button();
            play_event_sound(VIBE_STICK_SOUND_ERROR,
                             "air mouse read error");
        }
        return;
    }
    if (s_imu_error) {
        ESP_LOGI(TAG, "MPU6886 input recovered");
    }
    s_imu_error = false;
    s_imu_read_errors = 0;

    vibe_air_mouse_sample_t sample = {0};
    for (size_t axis = 0; axis < 3; ++axis) {
        sample.accel_g[axis] = motion_sample.accel_g[axis];
        sample.gyro_dps[axis] = motion_sample.gyro_dps[axis];
    }
    float delta_seconds = s_last_imu_sample_ms > 0
                              ? (float)(current_ms - s_last_imu_sample_ms) /
                                    1000.0f
                              : (float)APP_LOOP_MS / 1000.0f;
    s_last_imu_sample_ms = current_ms;

    vibe_air_mouse_output_t output = {0};
    bool ready = vibe_air_mouse_update(&s_air_mouse, &sample,
                                       delta_seconds, &output);
    if (!s_air_mouse_calibrated &&
        current_ms - s_last_calibration_log_ms >=
            AIR_MOUSE_CALIBRATION_LOG_MS) {
        s_last_calibration_log_ms = current_ms;
        ESP_LOGI(TAG, "air mouse calibration progress=%u/50",
                 vibe_air_mouse_calibration_progress(&s_air_mouse));
    }
    if (!s_air_mouse_calibrated && ready &&
        vibe_air_mouse_calibrated(&s_air_mouse)) {
        s_air_mouse_calibrated = true;
        vibe_bt_status_ui_set_air_mouse(true, true);
        register_activity();
        ESP_LOGI(TAG,
                 "mode=AIR_MOUSE_ACTIVE gyro_bias=(%.2f,%.2f,%.2f)",
                 s_air_mouse.gyro_bias[0], s_air_mouse.gyro_bias[1],
                 s_air_mouse.gyro_bias[2]);
    }
    if (!ready || !s_air_mouse_calibrated) {
        return;
    }

    vibe_bt_composite_state_t state = bt_state();
    if (output.dx != 0 || output.dy != 0) {
        if (state.hid_connected) {
            err = vibe_bt_composite_send_mouse(
                output.dx, output.dy, s_air_mouse_left_down);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "air mouse report failed: %s",
                         esp_err_to_name(err));
            }
        }
        register_activity();
    }
    if (output.wheel != 0) {
        if (state.hid_connected) {
            err = vibe_bt_composite_send_scroll(
                output.wheel, s_air_mouse_left_down);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wheel %s failed: %s",
                         output.wheel > 0 ? "up" : "down",
                         esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "air mouse wheel=%s",
                         output.wheel > 0 ? "UP" : "DOWN");
            }
        }
        register_activity();
    }
    if (output.motion_started) {
        register_activity();
        play_event_sound(VIBE_STICK_SOUND_JOYSTICK_START,
                         "air mouse intentional motion");
    }
}

static void update_status(void)
{
    vibe_bt_composite_state_t state = bt_state();
    if (state.pairing && (state.hid_connected || state.hfp_connected)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_composite_end_pairing());
        s_pairing_deadline_ms = 0;
        state = bt_state();
    }
    if (!state.hid_connected) {
        s_air_mouse_left_down = false;
    }
    if (s_imu_error) {
        vibe_bt_status_ui_set(VIBE_BT_UI_ERROR, s_minijoy_ready);
    } else if (atomic_load(&s_capture_active)) {
        vibe_bt_status_ui_set(VIBE_BT_UI_RECORDING, false);
    } else if (state.pairing) {
        vibe_bt_status_ui_set(VIBE_BT_UI_PAIRING, s_minijoy_ready);
    } else if (state.hid_connected || state.hfp_connected) {
        vibe_bt_status_ui_set(VIBE_BT_UI_CONNECTED, s_minijoy_ready);
    } else if (state.paired || state.hid_connected || state.hfp_connected) {
        vibe_bt_status_ui_set(VIBE_BT_UI_CONNECTING, s_minijoy_ready);
    } else {
        vibe_bt_status_ui_set(VIBE_BT_UI_WAITING, s_minijoy_ready);
    }

}

static void update_wake_input_guard(int64_t current_ms)
{
    if (!atomic_load(&s_wake_input_guard)) {
        return;
    }
    bool released = gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) != 0 &&
                    gpio_get_level(VIBE_BOARD_PIN_BUTTON_SIDE) != 0;
    if (!released) {
        s_wake_release_since_ms = 0;
        return;
    }
    if (s_wake_release_since_ms == 0) {
        s_wake_release_since_ms = current_ms;
        return;
    }
    if (current_ms - s_wake_release_since_ms >= WAKE_RELEASE_STABLE_MS) {
        atomic_store(&s_wake_input_guard, false);
        s_wake_release_since_ms = 0;
        s_last_activity_ms = current_ms;
        ESP_LOGI(TAG, "deep-sleep wake buttons released; input enabled");
    }
}

static bool deep_sleep_has_active_work(int64_t current_ms)
{
    vibe_bt_composite_state_t state = bt_state();
    return atomic_load(&s_capture_active) ||
           atomic_load(&s_wake_input_guard) || state.pairing ||
           s_pairing_deadline_ms > current_ms ||
           s_startup_pairing_due_ms > current_ms ||
           s_confirm_deadline_ms > current_ms;
}

static esp_err_t configure_deep_sleep_wake_sources(void)
{
    ESP_RETURN_ON_ERROR(
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL), TAG,
        "clear wake sources");
    ESP_RETURN_ON_ERROR(
        esp_sleep_enable_ext0_wakeup(VIBE_BOARD_PIN_BUTTON_FRONT, 0), TAG,
        "front button wake");
    return esp_sleep_enable_ext1_wakeup_io(
        1ULL << VIBE_BOARD_PIN_BUTTON_SIDE, ESP_EXT1_WAKEUP_ALL_LOW);
}

static bool enter_deep_sleep(void)
{
    if (gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0 ||
        gpio_get_level(VIBE_BOARD_PIN_BUTTON_SIDE) == 0) {
        ESP_LOGW(TAG, "deep sleep deferred: wake button is active");
        return false;
    }
    esp_err_t err = configure_deep_sleep_wake_sources();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep wake setup failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    bool resume_motion_on_error =
        s_imu_available && !vibe_motion_suspended();
    err = vibe_motion_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep IMU preparation failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    err = vibe_audio_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep audio preparation failed: %s",
                 esp_err_to_name(err));
        if (resume_motion_on_error) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_resume());
        }
        return false;
    }

    set_minijoy_led(MINIJOY_LED_OFF);
    close_minijoy();
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_bt_status_ui_prepare_deep_sleep());
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_set_external_5v(false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_prepare_deep_sleep());
    ESP_LOGI(TAG,
             "entering deep sleep idle_ms=%d wake_front=%d wake_side=%d",
             DEEP_SLEEP_IDLE_MS, (int)VIBE_BOARD_PIN_BUTTON_FRONT,
             (int)VIBE_BOARD_PIN_BUTTON_SIDE);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
    return true;
}

static void maybe_enter_deep_sleep(int64_t current_ms)
{
    if (s_last_activity_ms == 0 ||
        current_ms - s_last_activity_ms < DEEP_SLEEP_IDLE_MS ||
        deep_sleep_has_active_work(current_ms) ||
        (s_next_deep_sleep_attempt_ms > 0 &&
         current_ms < s_next_deep_sleep_attempt_ms)) {
        return;
    }
    s_next_deep_sleep_attempt_ms = current_ms + DEEP_SLEEP_RETRY_MS;

    bool usb_powered = false;
    esp_err_t power_err = vibe_board_usb_powered(&usb_powered);
    if (power_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep deferred: USB status unavailable: %s",
                 esp_err_to_name(power_err));
        return;
    }
    if (usb_powered) {
        return;
    }
    (void)enter_deep_sleep();
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
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    uint64_t ext1_status = esp_sleep_get_ext1_wakeup_status();
    bool woke_from_button = wake_cause == ESP_SLEEP_WAKEUP_EXT0 ||
                            wake_cause == ESP_SLEEP_WAKEUP_EXT1;
    if (woke_from_button) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            rtc_gpio_deinit(VIBE_BOARD_PIN_BUTTON_FRONT));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            rtc_gpio_deinit(VIBE_BOARD_PIN_BUTTON_SIDE));
        atomic_store(&s_wake_input_guard, true);
        ESP_LOGI(TAG, "deep-sleep wake cause=%d ext1=0x%llx",
                 (int)wake_cause, (unsigned long long)ext1_status);
    }
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

    const vibe_air_mouse_config_t air_mouse_config = {
        .horizontal_axis = 2,
        .horizontal_sign = -1,
        .horizontal_gain = 1.8f,
        .vertical_axis = 0,
        .vertical_sign = -1,
        .vertical_gain = 1.0f,
    };
    vibe_air_mouse_init(&s_air_mouse, &air_mouse_config);

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
    s_last_activity_ms = now_ms();
    open_minijoy();
    vibe_bt_composite_state_t initial_state = bt_state();
    if (!initial_state.paired) {
        s_startup_pairing_due_ms = now_ms() + STARTUP_PAIRING_DELAY_MS;
        ESP_LOGI(TAG, "startup pairing window scheduled");
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_bt_composite_request_reconnect());
        ESP_LOGI(TAG, "bonded host found; automatic reconnect started");
    }
    update_status();

    ESP_LOGI(TAG,
             "dedicated MiniJoy BT firmware ready version=%s mode=JOYSTICK_PTT",
             FIRMWARE_VERSION);
    for (;;) {
        int64_t current_ms = now_ms();
        update_wake_input_guard(current_ms);
        app_event_t event;
        while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
            handle_event(event);
        }
        poll_minijoy();
        current_ms = now_ms();
        poll_air_mouse(current_ms);
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
        vibe_bt_status_ui_tick(current_ms);
        maybe_enter_deep_sleep(current_ms);
        vTaskDelay(pdMS_TO_TICKS(APP_LOOP_MS));
    }
}
