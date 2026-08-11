#include "vibe_cardputer_air_mouse.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "vibe_air_mouse.h"
#include "vibe_board_profile.h"
#include "vibe_motion.h"

#define AIR_MOUSE_SAMPLE_MS 20
#define AIR_MOUSE_CALIBRATION_LOG_MS 1000
#define AIR_MOUSE_IMU_ERROR_LIMIT 5
#define AIR_MOUSE_NVS_NAMESPACE "vibe_prefs"
#define AIR_MOUSE_NVS_KEY "card_mouse_v1"
#define AIR_MOUSE_CALIBRATION_MAGIC 0x5643414du
#define AIR_MOUSE_CALIBRATION_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char board[16];
    vibe_air_mouse_calibration_t calibration;
} stored_calibration_t;

static const char *TAG = "card_air_mouse";
static vibe_card_air_mouse_config_t s_config;
static vibe_air_mouse_t s_mouse;
static atomic_bool s_enabled;
static bool s_consumed[4][14];
static int64_t s_last_sample_ms;
static int64_t s_last_calibration_log_ms;
static unsigned s_read_errors;

static void report_status(vibe_card_air_mouse_status_t status,
                          uint16_t progress)
{
    if (s_config.status) {
        s_config.status(status, progress, s_config.context);
    }
}

static esp_err_t erase_stored_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIR_MOUSE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, AIR_MOUSE_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIR_MOUSE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    stored_calibration_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, AIR_MOUSE_NVS_KEY, &stored, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(stored) ||
        stored.magic != AIR_MOUSE_CALIBRATION_MAGIC ||
        stored.version != AIR_MOUSE_CALIBRATION_VERSION ||
        stored.size != sizeof(stored) ||
        strncmp(stored.board, VIBE_BOARD_NAME, sizeof(stored.board)) != 0 ||
        !vibe_air_mouse_set_calibration(&s_mouse, &stored.calibration)) {
        ESP_LOGW(TAG, "stored calibration rejected");
        (void)erase_stored_calibration();
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "stored calibration loaded board=%s", stored.board);
    return ESP_OK;
}

static esp_err_t save_calibration(void)
{
    stored_calibration_t stored = {
        .magic = AIR_MOUSE_CALIBRATION_MAGIC,
        .version = AIR_MOUSE_CALIBRATION_VERSION,
        .size = sizeof(stored_calibration_t),
    };
    snprintf(stored.board, sizeof(stored.board), "%s", VIBE_BOARD_NAME);
    if (!vibe_air_mouse_get_calibration(&s_mouse, &stored.calibration)) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AIR_MOUSE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open calibration NVS");
    err = nvs_set_blob(handle, AIR_MOUSE_NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void stop_internal(bool notify)
{
    atomic_store(&s_enabled, false);
    vibe_pointer_client_release_all();
    vibe_air_mouse_reset_motion(&s_mouse);
    memset(s_consumed, 0, sizeof(s_consumed));
    s_last_sample_ms = 0;
    s_read_errors = 0;
    if (!s_config.keep_motion_active ||
        !s_config.keep_motion_active(s_config.context)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_suspend());
    }
    if (notify) {
        report_status(VIBE_CARD_AIR_MOUSE_DISABLED, 0);
    }
    ESP_LOGI(TAG, "disabled");
}

static void set_enabled(bool enabled)
{
    if (!enabled) {
        stop_internal(true);
        return;
    }
    if (!vibe_motion_available()) {
        report_status(VIBE_CARD_AIR_MOUSE_UNAVAILABLE, 0);
        return;
    }
    if (s_config.busy && s_config.busy(s_config.context)) {
        report_status(VIBE_CARD_AIR_MOUSE_BUSY, 0);
        return;
    }
    esp_err_t err = vibe_motion_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU resume failed: %s", esp_err_to_name(err));
        report_status(VIBE_CARD_AIR_MOUSE_FAILED, 0);
        return;
    }
    if (s_config.release_keyboard) {
        s_config.release_keyboard(s_config.context);
    }
    vibe_pointer_client_release_all();
    vibe_air_mouse_reset_motion(&s_mouse);
    s_last_sample_ms = 0;
    s_last_calibration_log_ms = 0;
    s_read_errors = 0;
    atomic_store(&s_enabled, true);
    report_status(vibe_air_mouse_calibrated(&s_mouse)
                      ? VIBE_CARD_AIR_MOUSE_READY
                      : VIBE_CARD_AIR_MOUSE_CALIBRATING,
                  vibe_air_mouse_calibration_progress(&s_mouse));
    ESP_LOGI(TAG, "enabled calibrated=%d",
             vibe_air_mouse_calibrated(&s_mouse));
}

esp_err_t vibe_cardputer_air_mouse_init(
    const vibe_card_air_mouse_config_t *config)
{
    if (!config || !config->pointer.post || !config->pointer.online ||
        !config->pointer.path) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    vibe_air_mouse_init(&s_mouse,
                        &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    esp_err_t load_err = load_calibration();
    if (load_err != ESP_OK && load_err != ESP_ERR_NVS_NOT_FOUND &&
        load_err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "calibration load skipped: %s",
                 esp_err_to_name(load_err));
    }
    return vibe_pointer_client_init(&config->pointer);
}

bool vibe_cardputer_air_mouse_handle_key(const vibe_key_event_t *event)
{
    if (!event || event->row >= 4 || event->column >= 14) {
        return false;
    }
    const uint8_t row = event->row;
    const uint8_t col = event->column;
    if (s_consumed[row][col]) {
        if (!event->pressed) {
            if (row == 3 && col == 13) {
                vibe_pointer_client_set_button(VIBE_POINTER_BUTTON_LEFT,
                                               false);
            } else if (row == 2 && col == 13) {
                vibe_pointer_client_set_button(VIBE_POINTER_BUTTON_RIGHT,
                                               false);
            }
            s_consumed[row][col] = false;
        }
        return true;
    }

    if (event->pressed && event->fn && row == 3 && col == 9) {
        s_consumed[row][col] = true;
        set_enabled(!atomic_load(&s_enabled));
        return true;
    }
    if (!event->pressed || !atomic_load(&s_enabled) || event->fn ||
        event->shift || event->ctrl || event->alt || event->opt ||
        event->hid_modifiers != 0) {
        return false;
    }
    uint8_t button = 0;
    if (row == 3 && col == 13) {
        button = VIBE_POINTER_BUTTON_LEFT;
    } else if (row == 2 && col == 13) {
        button = VIBE_POINTER_BUTTON_RIGHT;
    } else {
        return false;
    }
    s_consumed[row][col] = true;
    if (s_config.release_keyboard) {
        s_config.release_keyboard(s_config.context);
    }
    vibe_pointer_client_set_button(button, true);
    if (s_config.activity) {
        s_config.activity(s_config.context);
    }
    return true;
}

void vibe_cardputer_air_mouse_poll(int64_t now_ms)
{
    if (!atomic_load(&s_enabled)) {
        return;
    }
    if (s_config.busy && s_config.busy(s_config.context)) {
        vibe_pointer_client_release_all();
        vibe_air_mouse_reset_motion(&s_mouse);
        s_last_sample_ms = now_ms;
        return;
    }
    if (s_last_sample_ms != 0 &&
        now_ms - s_last_sample_ms < AIR_MOUSE_SAMPLE_MS) {
        return;
    }
    const float delta_seconds =
        s_last_sample_ms == 0
            ? AIR_MOUSE_SAMPLE_MS / 1000.0f
            : (now_ms - s_last_sample_ms) / 1000.0f;
    s_last_sample_ms = now_ms;

    vibe_motion_sample_t raw = {0};
    esp_err_t err = vibe_motion_read_raw_sample(&raw);
    if (err != ESP_OK) {
        if (++s_read_errors >= AIR_MOUSE_IMU_ERROR_LIMIT) {
            ESP_LOGE(TAG, "IMU read failed repeatedly: %s",
                     esp_err_to_name(err));
            stop_internal(false);
            report_status(VIBE_CARD_AIR_MOUSE_FAILED, 0);
        }
        return;
    }
    s_read_errors = 0;

    vibe_air_mouse_sample_t sample = {0};
    memcpy(sample.accel_g, raw.accel_g, sizeof(sample.accel_g));
    memcpy(sample.gyro_dps, raw.gyro_dps, sizeof(sample.gyro_dps));
    const bool was_calibrated = vibe_air_mouse_calibrated(&s_mouse);
    vibe_air_mouse_output_t output = {0};
    const bool ready =
        vibe_air_mouse_update(&s_mouse, &sample, delta_seconds, &output);
    if (!was_calibrated && vibe_air_mouse_calibrated(&s_mouse)) {
        esp_err_t save_err = save_calibration();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "calibration save failed: %s",
                     esp_err_to_name(save_err));
        }
        report_status(VIBE_CARD_AIR_MOUSE_READY,
                      VIBE_AIR_MOUSE_CALIBRATION_SAMPLES);
        ESP_LOGI(TAG, "calibration complete and persisted");
    } else if (!ready &&
               now_ms - s_last_calibration_log_ms >=
                   AIR_MOUSE_CALIBRATION_LOG_MS) {
        const uint16_t progress =
            vibe_air_mouse_calibration_progress(&s_mouse);
        report_status(VIBE_CARD_AIR_MOUSE_CALIBRATING, progress);
        ESP_LOGI(TAG, "calibration %u/%u; keep still", (unsigned)progress,
                 (unsigned)VIBE_AIR_MOUSE_CALIBRATION_SAMPLES);
        s_last_calibration_log_ms = now_ms;
    }
    if (!ready) {
        return;
    }
    if (output.dx != 0 || output.dy != 0 || output.wheel != 0) {
        vibe_pointer_client_report(output.dx, output.dy, output.wheel);
        if (s_config.activity) {
            s_config.activity(s_config.context);
        }
    }
}

void vibe_cardputer_air_mouse_stop(void)
{
    if (atomic_load(&s_enabled) || vibe_pointer_client_buttons() != 0) {
        stop_internal(false);
    }
}

bool vibe_cardputer_air_mouse_enabled(void)
{
    return atomic_load(&s_enabled);
}

bool vibe_cardputer_air_mouse_apply_settings(
    const vibe_card_air_mouse_settings_t *settings)
{
    if (!settings || settings->pointer_speed < 0.5f ||
        settings->pointer_speed > 2.5f || settings->wheel_speed < 0.5f ||
        settings->wheel_speed > 2.0f || settings->pointer_deadzone_dps < 1.0f ||
        settings->pointer_deadzone_dps > 6.0f ||
        settings->wheel_deadzone_dps < 2.0f ||
        settings->wheel_deadzone_dps > 10.0f) {
        return false;
    }
    vibe_air_mouse_config_t config = VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG;
    config.horizontal_sign *= settings->invert_horizontal ? -1 : 1;
    config.vertical_sign *= settings->invert_vertical ? -1 : 1;
    config.wheel_sign *= settings->invert_scroll ? -1 : 1;
    config.horizontal_gain *= settings->pointer_speed;
    config.vertical_gain *= settings->pointer_speed;
    config.pointer_deadzone_dps = settings->pointer_deadzone_dps;
    config.wheel_deadzone_dps = settings->wheel_deadzone_dps;
    config.wheel_degrees_per_tick =
        VIBE_AIR_MOUSE_DEFAULT_WHEEL_DEGREES_PER_TICK /
        settings->wheel_speed;
    return vibe_air_mouse_set_config(&s_mouse, &config);
}
