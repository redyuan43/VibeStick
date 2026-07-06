#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_motion.h"
#include "vibe_stick_pet_assets.h"
#include "vibe_stick_config.h"
#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#define LCD_HOST VIBE_BOARD_LCD_HOST
#define LCD_H_RES VIBE_BOARD_LCD_H_RES
#define LCD_V_RES VIBE_BOARD_LCD_V_RES
#define LCD_X_GAP VIBE_BOARD_LCD_X_GAP
#define LCD_Y_GAP VIBE_BOARD_LCD_Y_GAP
#define LCD_PIXEL_CLOCK_HZ VIBE_BOARD_LCD_PIXEL_CLOCK_HZ
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX 255
#define LCD_BACKLIGHT_DEFAULT VIBE_BOARD_LCD_BACKLIGHT_DEFAULT
#define LCD_BACKLIGHT_IDLE VIBE_BOARD_LCD_BACKLIGHT_IDLE
#define LCD_BACKLIGHT_OFF VIBE_BOARD_LCD_BACKLIGHT_OFF
#define LVGL_DRAW_BUF_LINES 24
#define LVGL_TICK_PERIOD_MS 10
#define BATTERY_FILL_MAX_WIDTH 20
#define RECORDING_UPLOAD_BATCH_CHUNKS 4
#define RECORDING_UPLOAD_BUFFER_BYTES 8192
#define RECORDING_UPLOAD_WAIT_MS 10000
#define RECORDING_START_TIMEOUT_MS 1200
#define OTA_READ_BUFFER_BYTES 4096
#define FIRMWARE_BUILD_ID __DATE__ " " __TIME__
#define VIBE_STICK_APP_CORE 0
#define VIBE_STICK_UI_CORE 1
#define VIBE_STICK_NETWORK_CORE 1
#define VIBE_STICK_IDLE_DIM_MS 30000
#define VIBE_STICK_IDLE_OFF_MS 60000
#define VIBE_STICK_DEEP_SLEEP_MS 600000
#define VIBE_STICK_IDLE_STATE_POLL_MS 60000
#define VIBE_STICK_BACKLIGHT_FADE_INTERVAL_MS 60
#define VIBE_STICK_BACKLIGHT_FADE_STEP 5
#define VIBE_STICK_PET_FAST_RESUME_MAX_MS 15000
#define RECORDING_RSSI_UNKNOWN -127
#define WIFI_PROFILE_NAMESPACE "vibe_wifi"
#define WIFI_PROFILE_BLOB_KEY "profiles"
#define WIFI_PROFILE_MAGIC 0x56425746u
#define WIFI_PROFILE_STORE_VERSION 1
#define WIFI_PROFILE_MAX_COUNT 4
#define WIFI_PROFILE_SSID_LEN 33
#define WIFI_PROFILE_PASSWORD_LEN 65
#define WIFI_PROFILE_RETRY_LIMIT 2
#define DEVICE_PREF_NAMESPACE "vibe_prefs"
#define DEVICE_PREF_RECORDING_MODE_KEY "rec_mode"

static const char *TAG = "vibe_stick";

typedef struct {
    char ssid[WIFI_PROFILE_SSID_LEN];
    char password[WIFI_PROFILE_PASSWORD_LEN];
} wifi_profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    wifi_profile_t profiles[WIFI_PROFILE_MAX_COUNT];
} wifi_profile_store_t;

#ifndef VIBE_STICK_WIFI_PROFILES
#define VIBE_STICK_WIFI_PROFILES \
    { { VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD } }
#endif

static const wifi_profile_t k_configured_wifi_profiles[] = VIBE_STICK_WIFI_PROFILES;

typedef enum {
    VIBE_STICK_EVENT_POLL_STATE,
    VIBE_STICK_EVENT_RECORDING_TOGGLE,
    VIBE_STICK_EVENT_DOUBLE_CLICK,
    VIBE_STICK_EVENT_LONG_START,
    VIBE_STICK_EVENT_LONG_STOP,
    VIBE_STICK_EVENT_PROVIDER_NEXT,
    VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE,
    VIBE_STICK_EVENT_MOTION_START,
    VIBE_STICK_EVENT_MOTION_STOP,
    VIBE_STICK_EVENT_OTA_CHECK,
} agent_event_type_t;

typedef struct {
    agent_event_type_t type;
} agent_event_t;

typedef enum {
    PROVIDER_CODEX = 0,
    PROVIDER_CLAUDE = 1,
    PROVIDER_COUNT,
} agent_provider_t;

typedef struct {
    agent_provider_t id;
    const char *key;
    const char *display_name;
    lv_color_t accent_color;
    bool enabled;
    bool implemented;
} agent_provider_config_t;

typedef struct {
    char time[8];
    bool wifi;
    bool ble;
    int battery;
    bool battery_charging;
    bool usb_powered;
    char codex_status[24];
    char project[40];
    int quota_5h;
    int quota_7d;
    bool quota_5h_valid;
    bool quota_7d_valid;
    char quota_updated_at[8];
    bool quota_stale;
    char alert_event_id[56];
    char alert_type[24];
    char alert_message[80];
} agent_state_t;

typedef struct {
    char status[24];
    char project[40];
    int quota_5h;
    int quota_7d;
    bool quota_5h_valid;
    bool quota_7d_valid;
    char quota_updated_at[8];
    bool quota_stale;
} provider_display_state_t;

typedef struct {
    char *data;
    int capacity;
    int used;
} http_response_capture_t;

typedef struct {
    bool available;
    char board[24];
    char version[48];
    char build_id[64];
    char elf_sha256[65];
    char url[160];
    int size;
} ota_manifest_t;

typedef enum {
    RECORDING_MODE_PUSH_TO_TALK,
    RECORDING_MODE_LIFT_TO_TALK,
} recording_mode_t;

typedef enum {
    DISPLAY_POWER_ACTIVE,
    DISPLAY_POWER_DIMMED,
    DISPLAY_POWER_OFF,
} display_power_state_t;

typedef struct {
    size_t upload_posts;
    size_t uploaded_bytes;
    size_t upload_failures;
    size_t read_failures;
    size_t read_timeouts;
    size_t max_pending_chunks;
    int64_t post_duration_total_ms;
    int64_t post_duration_min_ms;
    int64_t post_duration_max_ms;
    int start_rssi;
    int stop_rssi;
} recording_upload_stats_t;

static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_lvgl_lock;
static atomic_bool s_wifi_connected;
static wifi_profile_t s_wifi_profiles[WIFI_PROFILE_MAX_COUNT];
static size_t s_wifi_profile_count;
static size_t s_wifi_profile_index;
static int s_wifi_profile_retry_count;
static bool s_recording_overlay_visible;
static bool s_long_press_active;
static bool s_motion_recording_active;
static bool s_motion_calibrating;
static bool s_motion_lift_armed;
static bool s_motion_start_pending;
static bool s_tap_recording_active;
static char s_last_alert_event_id[56];
static char s_last_alert_type[24];
static bool s_alert_sound_baseline_ready;
static char s_recording_session_id[40];
static TaskHandle_t s_recording_upload_task;
static atomic_bool s_recording_upload_failed;
static TaskHandle_t s_recording_finalize_task;
static atomic_bool s_recording_finalize_active;
static char s_recording_finalize_event_name[32];
static TaskHandle_t s_ota_task;
static atomic_bool s_ota_in_progress;
static int64_t s_last_activity_ms;
static int64_t s_last_backlight_fade_ms;
static uint8_t s_current_backlight = LCD_BACKLIGHT_DEFAULT;
static display_power_state_t s_display_power_state = DISPLAY_POWER_ACTIVE;
static recording_upload_stats_t s_recording_upload_stats;
static uint8_t *s_pet_pixels;
static vibe_stick_pet_frame_id_t s_pet_current_frame = VIBE_STICK_PET_FRAME_COUNT;
static int64_t s_pet_next_frame_ms;
static int s_pet_sequence_index;
static int s_pet_sequence_key = -1;
static int s_pet_bob_step;

static lv_display_t *s_display;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_cap;
static lv_obj_t *s_battery_bolt;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_pet_image;
static bool s_ui_ready;
static bool s_woke_from_deep_sleep;
static bool s_wake_front_button_pending;
static bool s_pet_fast_resume_pending;
static int64_t s_pet_animation_resume_ms;
static lv_obj_t *s_recording_overlay;
static lv_obj_t *s_recording_wave_group;
static lv_obj_t *s_recording_wave_bars[5];
static lv_obj_t *s_recording_title;
static lv_obj_t *s_recording_hint;

static bool wifi_connected(void)
{
    return atomic_load(&s_wifi_connected);
}

static void set_wifi_connected(bool connected)
{
    atomic_store(&s_wifi_connected, connected);
}

static bool ota_in_progress(void)
{
    return atomic_load(&s_ota_in_progress);
}

static void set_ota_in_progress(bool in_progress)
{
    atomic_store(&s_ota_in_progress, in_progress);
}

static bool recording_upload_failed(void)
{
    return atomic_load(&s_recording_upload_failed);
}

static void set_recording_upload_failed(bool failed)
{
    atomic_store(&s_recording_upload_failed, failed);
}

static bool recording_finalize_active(void)
{
    return atomic_load(&s_recording_finalize_active);
}

static void set_recording_finalize_active(bool active)
{
    atomic_store(&s_recording_finalize_active, active);
}

static bool recording_network_busy(void)
{
    return vibe_audio_is_recording() ||
           s_recording_session_id[0] != '\0' ||
           s_recording_upload_task != NULL ||
           recording_finalize_active();
}

static agent_state_t s_state = {
    .time = "--:--",
    .wifi = false,
    .ble = false,
    .battery = 0,
    .battery_charging = false,
    .usb_powered = false,
    .codex_status = "OFFLINE",
    .project = "vibestick",
    .quota_5h = 0,
    .quota_7d = 0,
    .quota_5h_valid = false,
    .quota_7d_valid = false,
    .quota_updated_at = "",
    .quota_stale = false,
    .alert_event_id = "",
    .alert_type = "NONE",
    .alert_message = "",
};

static provider_display_state_t s_provider_states[PROVIDER_COUNT] = {
    [PROVIDER_CODEX] = {
        .status = "OFFLINE",
        .project = "vibestick",
        .quota_5h = 0,
        .quota_7d = 0,
        .quota_5h_valid = false,
        .quota_7d_valid = false,
        .quota_updated_at = "",
        .quota_stale = false,
    },
    [PROVIDER_CLAUDE] = {
        .status = "OFFLINE",
        .project = "vibestick",
        .quota_5h = 0,
        .quota_7d = 0,
        .quota_5h_valid = false,
        .quota_7d_valid = false,
        .quota_updated_at = "",
        .quota_stale = false,
    },
};

extern const lv_font_t vibe_stick_cn_16;
#define FONT_CN (&vibe_stick_cn_16)
#define FONT_ASCII (&lv_font_montserrat_10)

static const agent_provider_config_t s_provider_configs[] = {
    {
        .id = PROVIDER_CODEX,
        .key = "codex",
        .display_name = "Codex",
        .accent_color = LV_COLOR_MAKE(0x4d, 0x82, 0xff),
        .enabled = true,
        .implemented = true,
    },
    {
        .id = PROVIDER_CLAUDE,
        .key = "claude",
        .display_name = "Claude",
        .accent_color = LV_COLOR_MAKE(0xd9, 0x77, 0x57),
        .enabled = true,
        .implemented = true,
    },
};

static agent_provider_t s_current_provider = PROVIDER_CODEX;
static bool s_provider_manually_selected;
static recording_mode_t s_recording_mode = RECORDING_MODE_PUSH_TO_TALK;

static const lv_point_precise_t s_battery_bolt_points[] = {
    {3, 0},
    {1, 3},
    {3, 3},
    {2, 7},
    {6, 2},
    {4, 2},
};

static void render_state(void);
static void copy_json_string(cJSON *root, const char *key, char *target, size_t target_len);
static void register_activity(void);
static bool front_button_is_pressed(void);
static void update_power_saving(int64_t now_ms);
static void maybe_enter_deep_sleep(int64_t now_ms);

static bool queue_event(agent_event_type_t type)
{
    if (!s_event_queue) {
        return false;
    }
    agent_event_t event = {.type = type};
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE;
}

static const agent_provider_config_t *provider_config(agent_provider_t provider)
{
    for (size_t i = 0; i < sizeof(s_provider_configs) / sizeof(s_provider_configs[0]); ++i) {
        if (s_provider_configs[i].id == provider) {
            return &s_provider_configs[i];
        }
    }
    return &s_provider_configs[0];
}

static const agent_provider_config_t *current_provider_config(void)
{
    return provider_config(s_current_provider);
}

static provider_display_state_t *provider_display_state(agent_provider_t provider)
{
    if ((int)provider >= 0 && provider < PROVIDER_COUNT) {
        return &s_provider_states[provider];
    }
    return &s_provider_states[PROVIDER_CODEX];
}

static provider_display_state_t *current_provider_display_state(void)
{
    return provider_display_state(s_current_provider);
}

static bool provider_from_key(const char *key, agent_provider_t *provider)
{
    if (!key || key[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_provider_configs) / sizeof(s_provider_configs[0]); ++i) {
        if (strcmp(s_provider_configs[i].key, key) == 0) {
            if (provider) {
                *provider = s_provider_configs[i].id;
            }
            return true;
        }
    }
    return false;
}

static bool set_current_provider_from_key(const char *key)
{
    agent_provider_t provider = PROVIDER_CODEX;
    if (provider_from_key(key, &provider)) {
        s_current_provider = provider;
        return true;
    }
    return false;
}

static agent_provider_t next_enabled_provider(agent_provider_t current)
{
    const size_t count = sizeof(s_provider_configs) / sizeof(s_provider_configs[0]);
    size_t current_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s_provider_configs[i].id == current) {
            current_index = i;
            break;
        }
    }
    for (size_t offset = 1; offset <= count; ++offset) {
        const size_t candidate_index = (current_index + offset) % count;
        if (s_provider_configs[candidate_index].enabled) {
            return s_provider_configs[candidate_index].id;
        }
    }
    return PROVIDER_CODEX;
}

static void switch_provider(void)
{
    if (s_recording_overlay_visible) {
        ESP_LOGI(TAG, "provider switch ignored while overlay is visible");
        return;
    }
    s_current_provider = next_enabled_provider(s_current_provider);
    s_provider_manually_selected = true;
    const agent_provider_config_t *provider = current_provider_config();
    ESP_LOGI(TAG, "provider switched to %s", provider->key);
    render_state();
}

static const char *recording_mode_label(void)
{
    if (s_recording_mode == RECORDING_MODE_LIFT_TO_TALK && s_motion_calibrating) {
        return "CAL";
    }
    return s_recording_mode == RECORDING_MODE_LIFT_TO_TALK ? "LIFT" : "PTT";
}

static void reset_recording_mode_runtime_state(void)
{
    s_motion_recording_active = false;
    s_motion_calibrating = false;
    s_motion_lift_armed = false;
    s_motion_start_pending = false;
}

static esp_err_t save_recording_mode_preference(recording_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open device preference NVS for write");
    err = nvs_set_u8(handle, DEVICE_PREF_RECORDING_MODE_KEY, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "write recording mode preference");
    ESP_LOGI(TAG, "saved recording mode preference=%s", recording_mode_label());
    return ESP_OK;
}

static esp_err_t restore_recording_mode_preference(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open device preference NVS");

    uint8_t stored_mode = 0;
    err = nvs_get_u8(handle, DEVICE_PREF_RECORDING_MODE_KEY, &stored_mode);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read recording mode preference");

    if (stored_mode == (uint8_t)RECORDING_MODE_LIFT_TO_TALK) {
        if (!vibe_motion_available()) {
            ESP_LOGW(TAG, "stored lift recording mode ignored: IMU is not ready");
            s_recording_mode = RECORDING_MODE_PUSH_TO_TALK;
            reset_recording_mode_runtime_state();
            return ESP_OK;
        }
        err = vibe_motion_recalibrate();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stored lift recording mode calibration failed: %s",
                     esp_err_to_name(err));
            s_recording_mode = RECORDING_MODE_PUSH_TO_TALK;
            reset_recording_mode_runtime_state();
            return ESP_OK;
        }
        s_recording_mode = RECORDING_MODE_LIFT_TO_TALK;
        s_motion_calibrating = true;
        s_motion_lift_armed = false;
        s_motion_start_pending = false;
    } else {
        s_recording_mode = RECORDING_MODE_PUSH_TO_TALK;
        reset_recording_mode_runtime_state();
    }
    ESP_LOGI(TAG, "restored recording mode preference=%s", recording_mode_label());
    return ESP_OK;
}

static void toggle_recording_mode(void)
{
    register_activity();
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        ESP_LOGI(TAG, "recording mode switch ignored while recording");
        return;
    }
    if (s_recording_mode == RECORDING_MODE_PUSH_TO_TALK) {
        if (!vibe_motion_available()) {
            ESP_LOGW(TAG, "lift recording mode unavailable: IMU is not ready");
            return;
        }
        esp_err_t err = vibe_motion_recalibrate();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "lift recording mode calibration failed: %s", esp_err_to_name(err));
            return;
        }
        s_recording_mode = RECORDING_MODE_LIFT_TO_TALK;
        s_motion_calibrating = true;
        s_motion_lift_armed = false;
        s_motion_start_pending = false;
    } else {
        s_recording_mode = RECORDING_MODE_PUSH_TO_TALK;
        reset_recording_mode_runtime_state();
    }
    ESP_LOGI(TAG, "recording mode switched to %s", recording_mode_label());
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference(s_recording_mode));
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_APPROVAL);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording mode switch sound skipped: %s", esp_err_to_name(sound_err));
    }
    render_state();
}

static void lvgl_lock(void)
{
    if (s_lvgl_lock) {
        xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    }
}

static void lvgl_unlock(void)
{
    if (s_lvgl_lock) {
        xSemaphoreGive(s_lvgl_lock);
    }
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        lvgl_lock();
        uint32_t wait_ms = lv_timer_handler();
        lvgl_unlock();
        if (wait_ms < 5) {
            wait_ms = 5;
        }
        if (wait_ms > 250) {
            wait_ms = 250;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, width * height);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void set_backlight(uint8_t brightness)
{
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_set_lcd_brightness(brightness));
#endif
    s_current_backlight = brightness;
}

static void fade_backlight_toward(uint8_t target, int64_t now_ms)
{
    if (target == s_current_backlight) {
        return;
    }
    if (target > s_current_backlight) {
        set_backlight(target);
        s_last_backlight_fade_ms = now_ms;
        return;
    }
    if (now_ms - s_last_backlight_fade_ms < VIBE_STICK_BACKLIGHT_FADE_INTERVAL_MS) {
        return;
    }

    int next = (int)s_current_backlight - VIBE_STICK_BACKLIGHT_FADE_STEP;
    if (next < target) {
        next = target;
    }
    set_backlight((uint8_t)next);
    s_last_backlight_fade_ms = now_ms;
}

static bool external_powered(void)
{
    return s_state.battery_charging || s_state.usb_powered;
}

static bool display_should_stay_active(void)
{
    return external_powered() ||
           s_recording_overlay_visible ||
           vibe_audio_is_recording() ||
           s_recording_session_id[0] != '\0' ||
           s_tap_recording_active ||
           s_motion_recording_active ||
           s_motion_calibrating ||
           recording_finalize_active();
}

static bool deep_sleep_should_stay_awake(void)
{
    return display_should_stay_active() || ota_in_progress();
}

static bool front_button_is_pressed(void)
{
    return gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
}

static void register_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    if (s_display_power_state != DISPLAY_POWER_ACTIVE ||
        s_current_backlight != LCD_BACKLIGHT_DEFAULT) {
        s_display_power_state = DISPLAY_POWER_ACTIVE;
        set_backlight(LCD_BACKLIGHT_DEFAULT);
    }
}

static void update_power_saving(int64_t now_ms)
{
    if (s_last_activity_ms == 0) {
        s_last_activity_ms = now_ms;
    }
    display_power_state_t next_state = DISPLAY_POWER_ACTIVE;
    uint8_t target = LCD_BACKLIGHT_DEFAULT;
    if (!display_should_stay_active()) {
        const int64_t idle_ms = now_ms - s_last_activity_ms;
        if (idle_ms >= VIBE_STICK_IDLE_OFF_MS) {
            next_state = DISPLAY_POWER_OFF;
            target = LCD_BACKLIGHT_OFF;
        } else if (idle_ms >= VIBE_STICK_IDLE_DIM_MS) {
            next_state = DISPLAY_POWER_DIMMED;
            target = LCD_BACKLIGHT_IDLE;
        }
    }
    if (target != s_current_backlight) {
        fade_backlight_toward(target, now_ms);
    }
    s_display_power_state = next_state;
}

static void request_motion_recording_start(void)
{
    if (s_motion_recording_active) {
        s_motion_start_pending = false;
        return;
    }
    if (recording_network_busy()) {
        if (!s_motion_start_pending) {
            ESP_LOGI(TAG, "motion lift start deferred while recording network is busy");
        }
        s_motion_start_pending = true;
        s_motion_lift_armed = false;
        return;
    }
    if (queue_event(VIBE_STICK_EVENT_MOTION_START)) {
        s_motion_start_pending = false;
        s_motion_lift_armed = false;
    } else {
        ESP_LOGW(TAG, "motion lift start deferred because event queue is full");
        s_motion_start_pending = true;
        s_motion_lift_armed = false;
    }
}

static void configure_sleep_wake_gpio(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        return;
    }
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));
}

static uint64_t sleep_button_wake_mask(void)
{
    return (1ULL << VIBE_BOARD_PIN_BUTTON_FRONT) |
           (1ULL << VIBE_BOARD_PIN_BUTTON_SIDE);
}

static bool prepare_imu_deep_sleep_wake(uint64_t *wake_mask)
{
    if (s_recording_mode != RECORDING_MODE_LIFT_TO_TALK) {
        return true;
    }
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    esp_err_t err = vibe_motion_prepare_deep_sleep_wake();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: IMU wake prep failed: %s", esp_err_to_name(err));
        return false;
    }
    configure_sleep_wake_gpio(VIBE_BOARD_PIN_IMU_INT);
    *wake_mask |= 1ULL << VIBE_BOARD_PIN_IMU_INT;
    return true;
#else
    ESP_LOGI(TAG, "deep sleep skipped: %s lift mode has no verified IMU wake path",
             VIBE_BOARD_NAME);
    return false;
#endif
}

static void enter_deep_sleep(void)
{
    uint64_t wake_mask = sleep_button_wake_mask();
    configure_sleep_wake_gpio(VIBE_BOARD_PIN_BUTTON_FRONT);
    configure_sleep_wake_gpio(VIBE_BOARD_PIN_BUTTON_SIDE);

    if (!prepare_imu_deep_sleep_wake(&wake_mask)) {
        return;
    }

    ESP_LOGI(TAG, "entering deep sleep board=%s mode=%s wake_mask=0x%llx",
             VIBE_BOARD_NAME, recording_mode_label(), (unsigned long long)wake_mask);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
#if defined(CONFIG_IDF_TARGET_ESP32)
    gpio_num_t ext0_gpio = VIBE_BOARD_PIN_BUTTON_FRONT;
    if (s_recording_mode == RECORDING_MODE_LIFT_TO_TALK && VIBE_BOARD_PIN_IMU_INT != GPIO_NUM_NC) {
        ext0_gpio = VIBE_BOARD_PIN_IMU_INT;
    }
    configure_sleep_wake_gpio(ext0_gpio);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_ext0_wakeup(ext0_gpio, 0));
#else
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_ext1_wakeup_io(wake_mask,
                                                                  ESP_EXT1_WAKEUP_ANY_LOW));
#endif
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void maybe_enter_deep_sleep(int64_t now_ms)
{
    if (s_last_activity_ms == 0 ||
        deep_sleep_should_stay_awake() ||
        s_current_backlight != LCD_BACKLIGHT_OFF) {
        return;
    }
    if ((now_ms - s_last_activity_ms) < VIBE_STICK_DEEP_SLEEP_MS) {
        return;
    }
    enter_deep_sleep();
}

static void init_backlight(void)
{
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = VIBE_BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
#endif
    set_backlight(LCD_BACKLIGHT_DEFAULT);
}

static esp_err_t init_display(void)
{
    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = VIBE_BOARD_PIN_LCD_SCK,
        .mosi_io_num = VIBE_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = VIBE_BOARD_PIN_LCD_DC,
        .cs_gpio_num = VIBE_BOARD_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = NULL,
        .flags.sio_mode = VIBE_BOARD_LCD_SPI_SIO_MODE,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle),
                        TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = VIBE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP), TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(s_display, panel);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    size_t buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "lvgl buffer");
    lv_display_set_buffers(s_display, buf, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io_handle, &callbacks, s_display),
                        TAG, "panel cb");

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "tick start");

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 4096, NULL, 3, NULL,
                            VIBE_STICK_UI_CORE);
    return ESP_OK;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, int32_t width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static bool text_is_ascii(const char *text)
{
    if (!text) {
        return true;
    }
    while (*text) {
        if ((unsigned char)*text >= 0x80) {
            return false;
        }
        ++text;
    }
    return true;
}

static lv_obj_t *make_plain_obj(lv_obj_t *parent, int32_t w, int32_t h,
                                lv_color_t color, lv_opa_t opa, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static void set_battery_ui(int battery_value, bool charging, bool usb_powered)
{
    if (battery_value < 0) {
        battery_value = 0;
    } else if (battery_value > 100) {
        battery_value = 100;
    }

    char battery[8];
    if (battery_value > 0) {
        snprintf(battery, sizeof(battery), "%d%%", battery_value);
    } else {
        snprintf(battery, sizeof(battery), "--%%");
    }
    lv_label_set_text(s_battery_label, battery);

    int fill_width = battery_value > 0 ? (battery_value * 20) / 100 : 0;
    if (fill_width < 1 && battery_value > 0) {
        fill_width = 1;
    }

    const bool external_power = charging || usb_powered;
    const lv_color_t normal_color = lv_color_hex(0xf3f4f6);
    const lv_color_t charging_color = lv_color_hex(0x32d583);

    lv_obj_set_style_border_color(s_battery_icon, normal_color, 0);
    lv_obj_set_style_bg_color(s_battery_fill, external_power ? charging_color : normal_color, 0);
    lv_obj_set_style_bg_color(s_battery_cap, normal_color, 0);
    lv_obj_set_width(s_battery_fill, fill_width);

    if (s_battery_bolt) {
        if (external_power) {
            lv_obj_clear_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

typedef struct {
    const vibe_stick_pet_frame_id_t *frames;
    int frame_count;
    int frame_ms;
    int key;
} pet_sequence_t;

static const vibe_stick_pet_frame_id_t s_pet_idle_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_LEFT,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_RIGHT,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_BLINK_BOTH,
};

static const vibe_stick_pet_frame_id_t s_pet_running_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
    VIBE_STICK_PET_FRAME_CLOUDLING_BUILDING,
    VIBE_STICK_PET_FRAME_CLOUDLING_CONDUCTING,
};

static const vibe_stick_pet_frame_id_t s_pet_approval_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
    VIBE_STICK_PET_FRAME_CLOUDLING_REACT_DRAG,
};

static const vibe_stick_pet_frame_id_t s_pet_done_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_NOTIFICATION,
    VIBE_STICK_PET_FRAME_CLOUDLING_JUGGLING,
};

static const vibe_stick_pet_frame_id_t s_pet_error_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ERROR,
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
};

static const vibe_stick_pet_frame_id_t s_pet_sleep_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_TO_DOZING,
    VIBE_STICK_PET_FRAME_CLOUDLING_DOZING,
    VIBE_STICK_PET_FRAME_CLOUDLING_DOZING_TO_SLEEPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_SLEEPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_SLEEPING_TO_IDLE,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_TO_SLEEPING,
};

static bool set_pet_frame(vibe_stick_pet_frame_id_t frame)
{
    if (!s_pet_image || !s_pet_pixels || s_pet_current_frame == frame) {
        return true;
    }
    if (!vibe_stick_pet_decode_frame(frame, s_pet_pixels, VIBE_STICK_PET_PIXEL_BYTES)) {
        ESP_LOGW(TAG, "pet frame decode failed id=%d", (int)frame);
        return false;
    }
    s_pet_current_frame = frame;
    lv_obj_invalidate(s_pet_image);
    return true;
}

static pet_sequence_t pet_sequence_for_state(const char *status)
{
    if (strcmp(s_state.alert_type, "APPROVAL") == 0 ||
        strcmp(s_state.alert_type, "WAITING_APPROVAL") == 0 ||
        strcmp(s_state.alert_type, "PENDING_APPROVAL") == 0 ||
        strcmp(s_state.alert_type, "NEEDS_APPROVAL") == 0) {
        return (pet_sequence_t){s_pet_approval_frames,
                                sizeof(s_pet_approval_frames) / sizeof(s_pet_approval_frames[0]),
                                650, 2};
    }
    if (strcmp(s_state.alert_type, "ERROR") == 0 ||
        strcmp(s_state.alert_type, "FAILED") == 0 ||
        strcmp(s_state.alert_type, "FAILURE") == 0) {
        return (pet_sequence_t){s_pet_error_frames,
                                sizeof(s_pet_error_frames) / sizeof(s_pet_error_frames[0]),
                                750, 4};
    }
    if (strcmp(s_state.alert_type, "DONE") == 0 ||
        strcmp(s_state.alert_type, "COMPLETED") == 0 ||
        strcmp(s_state.alert_type, "SUCCESS") == 0) {
        return (pet_sequence_t){s_pet_done_frames,
                                sizeof(s_pet_done_frames) / sizeof(s_pet_done_frames[0]),
                                750, 3};
    }

    if (strcmp(status, "RUNNING") == 0) {
        return (pet_sequence_t){s_pet_running_frames,
                                sizeof(s_pet_running_frames) / sizeof(s_pet_running_frames[0]),
                                550, 1};
    }
    if (strcmp(status, "APPROVAL") == 0) {
        return (pet_sequence_t){s_pet_approval_frames,
                                sizeof(s_pet_approval_frames) / sizeof(s_pet_approval_frames[0]),
                                650, 2};
    }
    if (strcmp(status, "DONE") == 0) {
        return (pet_sequence_t){s_pet_done_frames,
                                sizeof(s_pet_done_frames) / sizeof(s_pet_done_frames[0]),
                                750, 3};
    }
    if (strcmp(status, "ERROR") == 0) {
        return (pet_sequence_t){s_pet_error_frames,
                                sizeof(s_pet_error_frames) / sizeof(s_pet_error_frames[0]),
                                750, 4};
    }
    if (strcmp(status, "OFFLINE") == 0 || strcmp(status, "UNIMPLEMENTED") == 0) {
        return (pet_sequence_t){s_pet_sleep_frames,
                                sizeof(s_pet_sleep_frames) / sizeof(s_pet_sleep_frames[0]),
                                900, 5};
    }
    return (pet_sequence_t){s_pet_idle_frames,
                            sizeof(s_pet_idle_frames) / sizeof(s_pet_idle_frames[0]),
                            220, 0};
}

static int pet_frame_delay_ms(pet_sequence_t sequence)
{
    if (sequence.key != 0) {
        return sequence.frame_ms;
    }
    return 1000 + (int)(esp_random() % 4001);
}

static void complete_pet_fast_resume(void)
{
    s_pet_fast_resume_pending = false;
    s_woke_from_deep_sleep = false;
}

static void update_pet_visual(void)
{
    if (s_display_power_state != DISPLAY_POWER_ACTIVE && !s_recording_overlay_visible) {
        return;
    }
    if (!s_pet_image) {
        return;
    }

    const int bob_offsets[] = {0, -2, -4, -2, 0, 2, 4, 2};
    const provider_display_state_t *display_state = current_provider_display_state();
    const agent_provider_config_t *provider = current_provider_config();
    const char *status = provider->implemented ? display_state->status : "UNIMPLEMENTED";
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_pet_fast_resume_pending) {
        if (now_ms < s_pet_animation_resume_ms) {
            return;
        }
        complete_pet_fast_resume();
    }

    pet_sequence_t sequence = pet_sequence_for_state(status);
    bool should_refresh_frame = false;
    if (sequence.key != s_pet_sequence_key) {
        s_pet_sequence_key = sequence.key;
        s_pet_sequence_index = sequence.key == 0
            ? (int)(esp_random() % (uint32_t)sequence.frame_count)
            : 0;
        s_pet_next_frame_ms = 0;
        should_refresh_frame = true;
    } else if (now_ms >= s_pet_next_frame_ms) {
        if (sequence.key == 0) {
            s_pet_sequence_index = (int)(esp_random() % (uint32_t)sequence.frame_count);
        } else {
            s_pet_sequence_index = (s_pet_sequence_index + 1) % sequence.frame_count;
        }
        should_refresh_frame = true;
    }
    if (should_refresh_frame) {
        set_pet_frame(sequence.frames[s_pet_sequence_index]);
        s_pet_next_frame_ms = now_ms + pet_frame_delay_ms(sequence);
    }
    lv_obj_align(s_pet_image, LV_ALIGN_CENTER, 0, 14 + bob_offsets[s_pet_bob_step]);
    s_pet_bob_step = (s_pet_bob_step + 1) %
                     (int)(sizeof(bob_offsets) / sizeof(bob_offsets[0]));
}

static void pet_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_pet_visual();
}

static void wave_bar_height_cb(void *obj, int32_t height)
{
    lv_obj_set_height((lv_obj_t *)obj, height);
}

static void stop_recording_wave(void)
{
    static const int heights[5] = {14, 22, 32, 22, 14};
    for (int i = 0; i < 5; ++i) {
        if (s_recording_wave_bars[i]) {
            lv_anim_delete(s_recording_wave_bars[i], NULL);
            lv_obj_set_height(s_recording_wave_bars[i], heights[i]);
        }
    }
}

static void start_recording_wave(void)
{
    static const int min_heights[5] = {10, 14, 18, 14, 10};
    static const int max_heights[5] = {24, 34, 48, 34, 24};
    stop_recording_wave();
    for (int i = 0; i < 5; ++i) {
        if (!s_recording_wave_bars[i]) {
            continue;
        }
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_recording_wave_bars[i]);
        lv_anim_set_values(&anim, min_heights[i], max_heights[i]);
        lv_anim_set_duration(&anim, 460);
        lv_anim_set_playback_duration(&anim, 460);
        lv_anim_set_delay(&anim, i * 70);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&anim, wave_bar_height_cb);
        lv_anim_start(&anim);
    }
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050608), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    s_wifi_label = make_label(screen, "WiFi", &lv_font_montserrat_10, lv_color_hex(0xf3f4f6), 38, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_LEFT, 9, 9);

    s_battery_label = make_label(screen, "--%", &lv_font_montserrat_10, lv_color_hex(0xf3f4f6), 28, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -35, 9);
    s_battery_icon = make_plain_obj(screen, 26, 13, lv_color_hex(0x000000), LV_OPA_TRANSP, 3);
    lv_obj_set_style_border_width(s_battery_icon, 1, 0);
    lv_obj_set_style_border_color(s_battery_icon, lv_color_hex(0xf3f4f6), 0);
    lv_obj_align(s_battery_icon, LV_ALIGN_TOP_RIGHT, -7, 9);
    s_battery_fill = make_plain_obj(s_battery_icon, 1, 9, lv_color_hex(0xf3f4f6), LV_OPA_COVER, 2);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);
    s_battery_bolt = lv_line_create(s_battery_icon);
    lv_line_set_points(s_battery_bolt, s_battery_bolt_points,
                       sizeof(s_battery_bolt_points) / sizeof(s_battery_bolt_points[0]));
    lv_obj_set_style_line_width(s_battery_bolt, 1, 0);
    lv_obj_set_style_line_color(s_battery_bolt, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_line_rounded(s_battery_bolt, true, 0);
    lv_obj_align(s_battery_bolt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_battery_bolt, LV_OBJ_FLAG_HIDDEN);
    s_battery_cap = make_plain_obj(screen, 2, 7, lv_color_hex(0xf3f4f6), LV_OPA_COVER, 1);
    lv_obj_align_to(s_battery_cap, s_battery_icon, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_mode_label = make_label(screen, "PTT", &lv_font_montserrat_10, lv_color_hex(0x8a9099), 34, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_MID, -2, 9);

    s_pet_image = lv_image_create(screen);
    s_pet_pixels = heap_caps_malloc(VIBE_STICK_PET_PIXEL_BYTES, MALLOC_CAP_8BIT);
    if (s_pet_pixels) {
        vibe_stick_pet_set_image_data(s_pet_pixels);
        if (vibe_stick_pet_decode_frame(VIBE_STICK_PET_FRAME_CLOUDLING_IDLE,
                                        s_pet_pixels, VIBE_STICK_PET_PIXEL_BYTES)) {
            s_pet_current_frame = VIBE_STICK_PET_FRAME_CLOUDLING_IDLE;
            lv_image_set_src(s_pet_image, &vibe_stick_pet_image);
        } else {
            lv_obj_add_flag(s_pet_image, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        ESP_LOGW(TAG, "pet image buffer allocation failed");
        lv_obj_add_flag(s_pet_image, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_pet_image, LV_ALIGN_CENTER, 0, 14);
    lv_timer_create(pet_timer_cb, 300, NULL);

    s_recording_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_recording_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_recording_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_recording_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_recording_overlay, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_opa(s_recording_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_recording_overlay, 0, 0);
    lv_obj_add_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);

    s_recording_wave_group = lv_obj_create(s_recording_overlay);
    lv_obj_remove_style_all(s_recording_wave_group);
    lv_obj_set_size(s_recording_wave_group, 82, 58);
    lv_obj_set_flex_flow(s_recording_wave_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_recording_wave_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_recording_wave_group, 6, 0);
    lv_obj_align(s_recording_wave_group, LV_ALIGN_CENTER, 0, -34);
    static const int initial_wave_heights[5] = {14, 22, 32, 22, 14};
    for (int i = 0; i < 5; ++i) {
        s_recording_wave_bars[i] = make_plain_obj(s_recording_wave_group, 6, initial_wave_heights[i],
                                                  lv_color_hex(0xf4f5f7), LV_OPA_COVER, 3);
    }

    s_recording_title = make_label(s_recording_overlay, "正在聆听", FONT_CN,
                                   lv_color_hex(0xf4f5f7), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_recording_title, FONT_CN, 0);
    lv_obj_align(s_recording_title, LV_ALIGN_CENTER, 0, 22);
    s_recording_hint = make_label(s_recording_overlay, "松开发送", FONT_CN,
                                  lv_color_hex(0x8b9098), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_recording_hint, FONT_CN, 0);
    lv_obj_align(s_recording_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void render_state(void)
{
    if (!s_ui_ready) {
        return;
    }
    lvgl_lock();
    const agent_provider_config_t *provider = current_provider_config();

    const bool connected = wifi_connected();
    lv_label_set_text(s_wifi_label, connected ? "WiFi" : "OFF");
    lv_obj_set_style_text_color(s_wifi_label,
                                connected ? lv_color_hex(0xf3f4f6) : lv_color_hex(0x686e78),
                                0);
    set_battery_ui(s_state.battery, s_state.battery_charging, s_state.usb_powered);
    lv_label_set_text(s_mode_label, recording_mode_label());
    lv_obj_set_style_text_color(s_mode_label,
                                s_recording_mode == RECORDING_MODE_LIFT_TO_TALK ?
                                    provider->accent_color : lv_color_hex(0x8a9099),
                                0);
    update_pet_visual();
    lvgl_unlock();
}

static void show_recording_overlay(const char *title, const char *hint, bool visible)
{
    lvgl_lock();
    if (visible) {
        if (title) {
            lv_obj_set_style_text_font(s_recording_title,
                                       text_is_ascii(title) ? FONT_ASCII : FONT_CN,
                                       0);
            lv_label_set_text(s_recording_title, title);
        }
        if (hint) {
            lv_obj_set_style_text_font(s_recording_hint,
                                       text_is_ascii(hint) ? FONT_ASCII : FONT_CN,
                                       0);
            lv_label_set_text(s_recording_hint, hint);
            if (hint[0] == '\0') {
                lv_obj_add_flag(s_recording_hint, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_recording_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_clear_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);
        start_recording_wave();
    } else {
        stop_recording_wave();
        lv_obj_add_flag(s_recording_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_recording_overlay_visible = visible;
    lvgl_unlock();
}

static bool sound_for_alert_type(const char *type, agent_sound_t *sound)
{
    if (strcmp(type, "DONE") == 0 ||
        strcmp(type, "COMPLETED") == 0 ||
        strcmp(type, "SUCCESS") == 0) {
        *sound = VIBE_STICK_SOUND_DONE;
        return true;
    }
    if (strcmp(type, "ERROR") == 0 ||
        strcmp(type, "FAILED") == 0 ||
        strcmp(type, "FAILURE") == 0) {
        *sound = VIBE_STICK_SOUND_ERROR;
        return true;
    }
    if (strcmp(type, "APPROVAL") == 0 ||
        strcmp(type, "WAITING_APPROVAL") == 0 ||
        strcmp(type, "PENDING_APPROVAL") == 0 ||
        strcmp(type, "NEEDS_APPROVAL") == 0) {
        *sound = VIBE_STICK_SOUND_APPROVAL;
        return true;
    }
    return false;
}

static void remember_alert_sound_baseline(void)
{
    strlcpy(s_last_alert_event_id, s_state.alert_event_id, sizeof(s_last_alert_event_id));
    strlcpy(s_last_alert_type, s_state.alert_type, sizeof(s_last_alert_type));
    s_alert_sound_baseline_ready = true;
}

static bool should_play_alert_sound(void)
{
    agent_sound_t ignored;
    const bool target = sound_for_alert_type(s_state.alert_type, &ignored);

    if (!s_alert_sound_baseline_ready) {
        remember_alert_sound_baseline();
        return false;
    }

    if (!target) {
        remember_alert_sound_baseline();
        return false;
    }

    bool should_play = false;
    if (s_state.alert_event_id[0] != '\0') {
        should_play = strcmp(s_last_alert_event_id, s_state.alert_event_id) != 0;
    } else {
        should_play = strcmp(s_last_alert_type, s_state.alert_type) != 0;
    }
    remember_alert_sound_baseline();
    return should_play;
}

static void maybe_handle_alert(void)
{
    agent_sound_t sound;
    if (!sound_for_alert_type(s_state.alert_type, &sound)) {
        (void)should_play_alert_sound();
        return;
    }
    if (!should_play_alert_sound()) {
        return;
    }
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        ESP_LOGI(TAG, "skip alert sound while recording overlay is active type=%s",
                 s_state.alert_type);
        return;
    }

    esp_err_t err = vibe_audio_play_sound(sound);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "alert sound skipped type=%s err=%s",
                 s_state.alert_type, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "alert type=%s project=%s message=%s",
             s_state.alert_type, s_state.project, s_state.alert_message);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_response_capture_t *capture = (http_response_capture_t *)evt->user_data;
    if (!capture->data || capture->capacity <= 0 || capture->used >= capture->capacity - 1) {
        return ESP_OK;
    }

    int remaining = capture->capacity - 1 - capture->used;
    int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
    memcpy(capture->data + capture->used, evt->data, copy_len);
    capture->used += copy_len;
    capture->data[capture->used] = '\0';
    return ESP_OK;
}

static esp_err_t http_request_timeout(const char *method, const char *path, const char *body,
                                      char *response, int response_len, int timeout_ms)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s", VIBE_STICK_BRIDGE_HOST, VIBE_STICK_BRIDGE_PORT, path);
    http_response_capture_t capture = {
        .data = response,
        .capacity = response_len,
        .used = 0,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", FIRMWARE_BUILD_ID);
    if (strlen(VIBE_STICK_BRIDGE_TOKEN) > 0) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && response && response_len > 0 && capture.used == 0) {
        ESP_LOGW(TAG, "http %s %s status=%d empty response", method, path, status_code);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t http_request(const char *method, const char *path, const char *body,
                              char *response, int response_len)
{
    return http_request_timeout(method, path, body, response, response_len, 2500);
}

static esp_err_t http_post_binary(const char *path, const uint8_t *body, size_t body_len,
                                  char *response, int response_len)
{
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", VIBE_STICK_BRIDGE_HOST, VIBE_STICK_BRIDGE_PORT, path);
    http_response_capture_t capture = {
        .data = response,
        .capacity = response_len,
        .used = 0,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", FIRMWARE_BUILD_ID);
    if (strlen(VIBE_STICK_BRIDGE_TOKEN) > 0) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Vibe-Stick-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Vibe-Stick-Channels", "1");
    esp_http_client_set_header(client, "X-Vibe-Stick-Bits-Per-Sample", "16");
    esp_http_client_set_post_field(client, (const char *)body, body_len);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && response && response_len > 0 && capture.used == 0) {
        ESP_LOGW(TAG, "http POST %s status=%d empty response", path, status_code);
    }
    esp_http_client_cleanup(client);
    return err;
}

static bool parse_ota_manifest(const char *json, ota_manifest_t *manifest)
{
    if (!json || !manifest) {
        return false;
    }
    memset(manifest, 0, sizeof(*manifest));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }

    cJSON *available = cJSON_GetObjectItemCaseSensitive(root, "available");
    manifest->available = cJSON_IsBool(available) ? cJSON_IsTrue(available) : false;
    copy_json_string(root, "board", manifest->board, sizeof(manifest->board));
    copy_json_string(root, "version", manifest->version, sizeof(manifest->version));
    copy_json_string(root, "build_id", manifest->build_id, sizeof(manifest->build_id));
    copy_json_string(root, "elf_sha256", manifest->elf_sha256, sizeof(manifest->elf_sha256));
    copy_json_string(root, "url", manifest->url, sizeof(manifest->url));
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(size)) {
        manifest->size = size->valueint;
    }
    cJSON_Delete(root);
    return manifest->available;
}

static void build_bridge_url(const char *path_or_url, char *url, size_t url_len)
{
    if (!path_or_url || !url || url_len == 0) {
        return;
    }
    if (strncmp(path_or_url, "http://", 7) == 0 ||
        strncmp(path_or_url, "https://", 8) == 0) {
        strlcpy(url, path_or_url, url_len);
        return;
    }
    snprintf(url, url_len, "http://%s:%d%s", VIBE_STICK_BRIDGE_HOST,
             VIBE_STICK_BRIDGE_PORT, path_or_url);
}

static bool ota_manifest_is_new(const ota_manifest_t *manifest)
{
    if (!manifest || !manifest->available) {
        return false;
    }
    if (strcmp(manifest->board, VIBE_BOARD_NAME) != 0) {
        ESP_LOGW(TAG, "OTA manifest board mismatch got=%s expected=%s",
                 manifest->board, VIBE_BOARD_NAME);
        return false;
    }
    if (manifest->elf_sha256[0] != '\0') {
        const char *current_elf_sha256 = esp_app_get_elf_sha256_str();
        size_t current_len = strlen(current_elf_sha256);
        if (current_len > 0 &&
            strncmp(manifest->elf_sha256, current_elf_sha256, current_len) == 0) {
            ESP_LOGI(TAG, "OTA manifest is current elf_sha256=%.12s current=%s",
                     manifest->elf_sha256, current_elf_sha256);
            return false;
        }
        ESP_LOGI(TAG, "OTA manifest is new elf_sha256=%.12s current=%.12s",
                 manifest->elf_sha256, current_elf_sha256);
        return true;
    }
    if (manifest->build_id[0] == '\0') {
        ESP_LOGW(TAG, "OTA manifest missing build_id and elf_sha256");
        return false;
    }
    if (strcmp(manifest->build_id, FIRMWARE_BUILD_ID) == 0) {
        ESP_LOGI(TAG, "OTA manifest is current build_id=%s", manifest->build_id);
        return false;
    }
    return true;
}

static esp_err_t perform_ota_update(const ota_manifest_t *manifest)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(update_partition != NULL, ESP_ERR_NOT_FOUND, TAG, "no OTA partition");
    if (manifest->size <= 0 || manifest->size > (int)update_partition->size) {
        ESP_LOGW(TAG, "OTA image size invalid size=%d partition=%u",
                 manifest->size, (unsigned)update_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    char default_path[80];
    const char *path_or_url = manifest->url;
    if (!path_or_url || path_or_url[0] == '\0') {
        snprintf(default_path, sizeof(default_path), "%s?board=%s",
                 VIBE_STICK_OTA_BIN_PATH, VIBE_BOARD_NAME);
        path_or_url = default_path;
    }

    char url[224];
    build_bridge_url(path_or_url, url, sizeof(url));
    ESP_LOGI(TAG, "OTA update available board=%s version=%s build=%s size=%d partition=%s",
             manifest->board, manifest->version, manifest->build_id, manifest->size,
             update_partition->label);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "OTA http init");
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", FIRMWARE_BUILD_ID);
    if (strlen(VIBE_STICK_BRIDGE_TOKEN) > 0) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", VIBE_STICK_BRIDGE_TOKEN);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGW(TAG, "OTA bin status=%d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length > 0 && content_length > (int64_t)update_partition->size) {
        ESP_LOGW(TAG, "OTA content too large length=%lld partition=%u",
                 (long long)content_length, (unsigned)update_partition->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t *buffer = heap_caps_malloc(OTA_READ_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (!buffer) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (true) {
        int data_read = esp_http_client_read(client, (char *)buffer, OTA_READ_BUFFER_BYTES);
        if (data_read < 0) {
            ESP_LOGW(TAG, "OTA read failed");
            err = ESP_FAIL;
            break;
        }
        if (data_read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_OK;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        err = esp_ota_write(ota_handle, buffer, data_read);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total_read += data_read;
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    heap_caps_free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }
    if (!complete) {
        esp_ota_abort(ota_handle);
        return ESP_ERR_INVALID_SIZE;
    }
    if (manifest->size > 0 && total_read != manifest->size) {
        ESP_LOGW(TAG, "OTA size mismatch read=%d manifest=%d", total_read, manifest->size);
        esp_ota_abort(ota_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OTA complete bytes=%d; restarting", total_read);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void ota_check_task(void *arg)
{
    (void)arg;
    char path[80];
    char response[768] = {0};
    ota_manifest_t manifest;

    snprintf(path, sizeof(path), "%s?board=%s", VIBE_STICK_OTA_MANIFEST_PATH, VIBE_BOARD_NAME);
    ESP_LOGD(TAG, "OTA check start path=%s", path);
    esp_err_t err = http_request_timeout("GET", path, NULL, response, sizeof(response), 5000);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "OTA manifest check failed: %s", esp_err_to_name(err));
    } else if (!parse_ota_manifest(response, &manifest)) {
        ESP_LOGD(TAG, "OTA manifest unavailable");
    } else if (!ota_manifest_is_new(&manifest)) {
        ESP_LOGD(TAG, "OTA no update board=%s build=%s", manifest.board, manifest.build_id);
    } else {
        show_recording_overlay("OTA update", "", true);
        err = perform_ota_update(&manifest);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA update failed: %s", esp_err_to_name(err));
            show_recording_overlay("OTA failed", "", true);
            vTaskDelay(pdMS_TO_TICKS(1200));
            show_recording_overlay(NULL, NULL, false);
        }
    }

    set_ota_in_progress(false);
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

static void start_ota_check_task(void)
{
    if (!wifi_connected() || ota_in_progress() || recording_network_busy()) {
        ESP_LOGD(TAG, "OTA check skipped wifi=%d ota=%d recording=%d",
                 wifi_connected(), ota_in_progress(), recording_network_busy());
        return;
    }
    set_ota_in_progress(true);
    BaseType_t ok = xTaskCreatePinnedToCore(ota_check_task, "ota_check", 8192, NULL, 3,
                                            &s_ota_task, VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        set_ota_in_progress(false);
        s_ota_task = NULL;
        ESP_LOGW(TAG, "OTA task create failed");
    }
}

static void copy_json_string(cJSON *root, const char *key, char *target, size_t target_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(target, item->valuestring, target_len);
    }
}

static bool json_percent_value(cJSON *item, int *value)
{
    if (cJSON_IsNumber(item)) {
        *value = item->valueint;
    } else if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(item->valuestring, &end, 10);
        if (!end || end == item->valuestring) {
            return false;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == '%') {
            end++;
        }
        if (*end != '\0') {
            return false;
        }
        *value = (int)parsed;
    } else {
        return false;
    }
    if (*value < 0) {
        *value = 0;
    } else if (*value > 100) {
        *value = 100;
    }
    return true;
}

static void parse_provider_fields(cJSON *source, provider_display_state_t *target)
{
    copy_json_string(source, "status", target->status, sizeof(target->status));
    copy_json_string(source, "project", target->project, sizeof(target->project));
    copy_json_string(source, "quota_updated_at", target->quota_updated_at, sizeof(target->quota_updated_at));

    cJSON *quota_5h = cJSON_GetObjectItemCaseSensitive(source, "quota_5h_remaining");
    cJSON *quota_7d = cJSON_GetObjectItemCaseSensitive(source, "quota_7d_remaining");
    cJSON *stale = cJSON_GetObjectItemCaseSensitive(source, "quota_stale");
    int quota_value = 0;
    target->quota_5h_valid = json_percent_value(quota_5h, &quota_value);
    if (target->quota_5h_valid) {
        target->quota_5h = quota_value;
    }
    target->quota_7d_valid = json_percent_value(quota_7d, &quota_value);
    if (target->quota_7d_valid) {
        target->quota_7d = quota_value;
    }
    target->quota_stale = cJSON_IsBool(stale) ? cJSON_IsTrue(stale) : false;
}

static void parse_provider_json(cJSON *state_root, cJSON *provider)
{
    char provider_key[16] = "";
    copy_json_string(provider, "id", provider_key, sizeof(provider_key));
    if (provider_key[0] == '\0') {
        copy_json_string(state_root, "active_provider", provider_key, sizeof(provider_key));
    }
    agent_provider_t provider_id = s_current_provider;
    if (provider_key[0] != '\0' && provider_from_key(provider_key, &provider_id)) {
        if (!s_provider_manually_selected) {
            s_current_provider = provider_id;
        }
    }

    provider_display_state_t *display_state = provider_display_state(provider_id);
    parse_provider_fields(provider, display_state);
    ESP_LOGI(TAG, "provider parsed key=%s status=%s q5=%s%d q7=%s%d stale=%d",
             provider_config(provider_id)->key,
             display_state->status,
             display_state->quota_5h_valid ? "" : "invalid:",
             display_state->quota_5h,
             display_state->quota_7d_valid ? "" : "invalid:",
             display_state->quota_7d,
             display_state->quota_stale);
}

static void parse_codex_json(cJSON *codex)
{
    provider_display_state_t *display_state = provider_display_state(PROVIDER_CODEX);
    parse_provider_fields(codex, display_state);
    ESP_LOGI(TAG, "codex parsed status=%s q5=%s%d q7=%s%d stale=%d",
             display_state->status,
             display_state->quota_5h_valid ? "" : "invalid:",
             display_state->quota_5h,
             display_state->quota_7d_valid ? "" : "invalid:",
             display_state->quota_7d,
             display_state->quota_stale);
}

static bool parse_state_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *state_root = root;
    cJSON *wrapped_state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsObject(wrapped_state)) {
        state_root = wrapped_state;
    }

    copy_json_string(state_root, "time", s_state.time, sizeof(s_state.time));
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(state_root, "wifi");
    cJSON *ble = cJSON_GetObjectItemCaseSensitive(state_root, "ble");
    s_state.wifi = cJSON_IsBool(wifi) ? cJSON_IsTrue(wifi) : s_state.wifi;
    s_state.ble = cJSON_IsBool(ble) ? cJSON_IsTrue(ble) : s_state.ble;

    cJSON *provider = cJSON_GetObjectItemCaseSensitive(state_root, "provider");
    cJSON *codex = cJSON_GetObjectItemCaseSensitive(state_root, "codex");
    if (cJSON_IsObject(provider)) {
        parse_provider_json(state_root, provider);
    } else {
        char active_provider[16] = "";
        copy_json_string(state_root, "active_provider", active_provider, sizeof(active_provider));
        if (active_provider[0] != '\0' && !s_provider_manually_selected) {
            set_current_provider_from_key(active_provider);
        }
    }
    if (cJSON_IsObject(codex)) {
        parse_codex_json(codex);
    }

    cJSON *alert = cJSON_GetObjectItemCaseSensitive(state_root, "alert");
    if (cJSON_IsObject(alert)) {
        copy_json_string(alert, "event_id", s_state.alert_event_id, sizeof(s_state.alert_event_id));
        copy_json_string(alert, "type", s_state.alert_type, sizeof(s_state.alert_type));
        copy_json_string(alert, "message", s_state.alert_message, sizeof(s_state.alert_message));
    }
    cJSON_Delete(root);
    return true;
}

static void poll_state(void)
{
    char response[1536] = {0};
    int battery_level = 0;
    if (vibe_board_battery_level(&battery_level) == ESP_OK) {
        s_state.battery = battery_level;
    }
    bool charging = false;
    bool usb_powered = false;
    bool power_read_ok = false;
    if (vibe_board_battery_charging(&charging) == ESP_OK) {
        s_state.battery_charging = charging;
        power_read_ok = true;
    }
    if (vibe_board_usb_powered(&usb_powered) == ESP_OK) {
        s_state.usb_powered = usb_powered;
        power_read_ok = true;
    }
    static bool last_power_logged = false;
    static bool last_charging = false;
    static bool last_usb_powered = false;
    if (power_read_ok &&
        (!last_power_logged ||
         last_charging != s_state.battery_charging ||
         last_usb_powered != s_state.usb_powered)) {
        ESP_LOGI(TAG, "power status battery=%d charging=%d usb=%d",
                 s_state.battery, s_state.battery_charging, s_state.usb_powered);
        last_power_logged = true;
        last_charging = s_state.battery_charging;
        last_usb_powered = s_state.usb_powered;
    }
    esp_err_t err = http_request("GET", VIBE_STICK_STATE_PATH, NULL, response, sizeof(response));
    if (err != ESP_OK || response[0] == '\0' || !parse_state_json(response)) {
        provider_display_state_t *display_state = current_provider_display_state();
        strlcpy(display_state->status, "OFFLINE", sizeof(display_state->status));
        s_state.wifi = wifi_connected();
        render_state();
        return;
    }
    complete_pet_fast_resume();
    render_state();
    maybe_handle_alert();
}

static void post_simple_event(const char *event_name, const char *path)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"event\":\"%s\",\"source\":\"%s\"}",
             event_name, VIBE_BOARD_EVENT_SOURCE);
    char response[512] = {0};
    const char *target_path = path ? path : VIBE_STICK_EVENT_PATH;
    esp_err_t err = http_request("POST", target_path, body, response, sizeof(response));
    if (err == ESP_OK && response[0] != '\0' && parse_state_json(response)) {
        complete_pet_fast_resume();
        render_state();
    }
}

static bool parse_recording_session_id(const char *json, char *session_id, size_t session_id_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *sid = cJSON_IsObject(recording) ? cJSON_GetObjectItemCaseSensitive(recording, "session_id") : NULL;
    bool ok = false;
    if (cJSON_IsString(sid) && sid->valuestring && sid->valuestring[0] != '\0') {
        strlcpy(session_id, sid->valuestring, session_id_len);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool is_recording_failure_status(const char *status)
{
    return strcmp(status, "transcription_failed") == 0 ||
           strcmp(status, "transcript_rejected") == 0 ||
           strcmp(status, "paste_failed") == 0 ||
           strcmp(status, "audio_failed") == 0 ||
           strcmp(status, "audio_skipped") == 0 ||
           strcmp(status, "start_failed") == 0 ||
           strcmp(status, "stop_failed") == 0;
}

static bool parse_recording_status(const char *json, char *status_text, size_t status_text_len)
{
    if (status_text_len > 0) {
        status_text[0] = '\0';
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *status = cJSON_IsObject(recording) ?
        cJSON_GetObjectItemCaseSensitive(recording, "status") : NULL;
    bool ok = false;
    if (cJSON_IsString(status) && status->valuestring) {
        strlcpy(status_text, status->valuestring, status_text_len);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void generate_recording_session_id(char *session_id, size_t session_id_len)
{
    if (session_id_len < 33) {
        if (session_id_len > 0) {
            session_id[0] = '\0';
        }
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        uint32_t value = esp_random();
        session_id[i] = hex[value & 0x0f];
    }
    session_id[32] = '\0';
}

static int current_wifi_rssi(void)
{
    if (!wifi_connected()) {
        return RECORDING_RSSI_UNKNOWN;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return RECORDING_RSSI_UNKNOWN;
    }
    return ap.rssi;
}

static void reset_recording_upload_stats(void)
{
    memset(&s_recording_upload_stats, 0, sizeof(s_recording_upload_stats));
    s_recording_upload_stats.post_duration_min_ms = -1;
    s_recording_upload_stats.start_rssi = current_wifi_rssi();
    s_recording_upload_stats.stop_rssi = RECORDING_RSSI_UNKNOWN;
}

static void record_upload_duration(int64_t duration_ms)
{
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    if (s_recording_upload_stats.post_duration_min_ms < 0 ||
        duration_ms < s_recording_upload_stats.post_duration_min_ms) {
        s_recording_upload_stats.post_duration_min_ms = duration_ms;
    }
    if (duration_ms > s_recording_upload_stats.post_duration_max_ms) {
        s_recording_upload_stats.post_duration_max_ms = duration_ms;
    }
    s_recording_upload_stats.post_duration_total_ms += duration_ms;
}

static void log_recording_diagnostics(void)
{
    vibe_audio_stats_t audio_stats = {0};
    vibe_audio_stats(&audio_stats);
    s_recording_upload_stats.stop_rssi = current_wifi_rssi();
    const int64_t avg_post_ms = s_recording_upload_stats.upload_posts > 0
                                    ? s_recording_upload_stats.post_duration_total_ms /
                                          (int64_t)s_recording_upload_stats.upload_posts
                                    : 0;
    const int64_t min_post_ms = s_recording_upload_stats.post_duration_min_ms >= 0
                                    ? s_recording_upload_stats.post_duration_min_ms
                                    : 0;
    ESP_LOGI(TAG,
             "recording diagnostics board=%s audio_read_chunks=%u audio_queued_chunks=%u "
             "audio_dropped_chunks=%u audio_dropped_bytes=%u upload_posts=%u uploaded_bytes=%u "
             "upload_failures=%u read_failures=%u read_timeouts=%u max_pending=%u "
             "post_ms_min=%lld post_ms_avg=%lld post_ms_max=%lld rssi_start=%d rssi_stop=%d",
             VIBE_BOARD_NAME,
             (unsigned)audio_stats.chunks_read,
             (unsigned)audio_stats.chunks_queued,
             (unsigned)audio_stats.chunks_dropped,
             (unsigned)audio_stats.bytes_dropped,
             (unsigned)s_recording_upload_stats.upload_posts,
             (unsigned)s_recording_upload_stats.uploaded_bytes,
             (unsigned)s_recording_upload_stats.upload_failures,
             (unsigned)s_recording_upload_stats.read_failures,
             (unsigned)s_recording_upload_stats.read_timeouts,
             (unsigned)s_recording_upload_stats.max_pending_chunks,
             (long long)min_post_ms,
             (long long)avg_post_ms,
             (long long)s_recording_upload_stats.post_duration_max_ms,
             s_recording_upload_stats.start_rssi,
             s_recording_upload_stats.stop_rssi);
}

static esp_err_t upload_recording_chunk(const uint8_t *audio, size_t audio_len)
{
    if (!audio || audio_len == 0 || s_recording_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s?session_id=%s&append=1",
             VIBE_STICK_RECORDING_AUDIO_PATH, s_recording_session_id);
    char response[512] = {0};
    esp_err_t err = http_post_binary(path, audio, audio_len, response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio chunk upload failed len=%u: %s",
                 (unsigned)audio_len, esp_err_to_name(err));
        return err;
    }
    (void)response;
    return ESP_OK;
}

static void recording_upload_task(void *arg)
{
    (void)arg;
    uint8_t *buffer = heap_caps_malloc(RECORDING_UPLOAD_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "recording upload buffer allocation failed");
        set_recording_upload_failed(true);
        s_recording_upload_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (vibe_audio_is_recording() || vibe_audio_pending_chunks() > 0) {
        size_t pending = vibe_audio_pending_chunks();
        if (pending > s_recording_upload_stats.max_pending_chunks) {
            s_recording_upload_stats.max_pending_chunks = pending;
        }
        size_t audio_len = 0;
        esp_err_t err = vibe_audio_read_batch(buffer, RECORDING_UPLOAD_BUFFER_BYTES, &audio_len,
                                              RECORDING_UPLOAD_BATCH_CHUNKS, 250);
        if (err == ESP_ERR_TIMEOUT) {
            s_recording_upload_stats.read_timeouts++;
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio read for upload failed: %s", esp_err_to_name(err));
            s_recording_upload_stats.read_failures++;
            set_recording_upload_failed(true);
            continue;
        }
        int64_t post_start_ms = esp_timer_get_time() / 1000;
        err = upload_recording_chunk(buffer, audio_len);
        int64_t post_duration_ms = esp_timer_get_time() / 1000 - post_start_ms;
        record_upload_duration(post_duration_ms);
        if (err != ESP_OK) {
            s_recording_upload_stats.upload_failures++;
            set_recording_upload_failed(true);
        } else {
            s_recording_upload_stats.upload_posts++;
            s_recording_upload_stats.uploaded_bytes += audio_len;
        }
    }

    heap_caps_free(buffer);
    ESP_LOGI(TAG, "recording upload task done posts=%u bytes=%u failures=%u failed=%d",
             (unsigned)s_recording_upload_stats.upload_posts,
             (unsigned)s_recording_upload_stats.uploaded_bytes,
             (unsigned)s_recording_upload_stats.upload_failures,
             recording_upload_failed());
    s_recording_upload_task = NULL;
    vTaskDelete(NULL);
}

static bool start_recording_upload_task(void)
{
    set_recording_upload_failed(false);
    reset_recording_upload_stats();
    s_recording_upload_task = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(recording_upload_task, "recording_upload", 6144,
                                            NULL, 4, &s_recording_upload_task,
                                            VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        set_recording_upload_failed(true);
        s_recording_upload_task = NULL;
        ESP_LOGW(TAG, "recording upload task create failed");
        return false;
    }
    return true;
}

static void wait_recording_upload_task(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RECORDING_UPLOAD_WAIT_MS);
    while (s_recording_upload_task != NULL && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_recording_upload_task != NULL) {
        ESP_LOGW(TAG, "recording upload task wait timeout");
        set_recording_upload_failed(true);
    }
}

static bool handle_recording_start(const char *event_name, const char *hint)
{
    register_activity();
    if (recording_network_busy() || s_tap_recording_active || s_motion_recording_active) {
        ESP_LOGI(TAG, "recording start ignored while already recording");
        return false;
    }
    generate_recording_session_id(s_recording_session_id, sizeof(s_recording_session_id));
    if (s_recording_session_id[0] == '\0') {
        ESP_LOGW(TAG, "recording start failed: no session id");
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    show_recording_overlay("正在连接", "", true);

    char body[192];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\","
             "\"audio_source\":\"%s\",\"session_id\":\"%s\"}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             VIBE_BOARD_AUDIO_SOURCE,
             s_recording_session_id);
    char response[1024] = {0};
    esp_err_t err = http_request_timeout("POST", VIBE_STICK_RECORDING_START_PATH, body, response,
                                         sizeof(response), RECORDING_START_TIMEOUT_MS);
    if (err == ESP_OK && response[0] != '\0') {
        char response_session_id[40] = {0};
        parse_recording_session_id(response, response_session_id, sizeof(response_session_id));
        if (response_session_id[0] != '\0' &&
            strcmp(response_session_id, s_recording_session_id) != 0) {
            ESP_LOGW(TAG, "bridge returned a different recording session id");
            strlcpy(s_recording_session_id, response_session_id, sizeof(s_recording_session_id));
        }
        if (parse_state_json(response)) {
            complete_pet_fast_resume();
            render_state();
        }
    } else {
        ESP_LOGW(TAG, "recording start bridge request failed: %s", esp_err_to_name(err));
        show_recording_overlay("连接失败", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        return false;
    }

    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_RECORDING_START);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording start sound skipped: %s", esp_err_to_name(sound_err));
    }

    esp_err_t audio_err = vibe_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording start failed: %s", esp_err_to_name(audio_err));
        show_recording_overlay("录音失败", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        return false;
    }
    if (!start_recording_upload_task()) {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        show_recording_overlay("发送失败", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        return false;
    }
    show_recording_overlay("正在聆听", hint, true);
    return true;
}

static void finish_recording_stop(const char *event_name)
{
    esp_err_t audio_err = vibe_audio_stop();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording stop failed: %s", esp_err_to_name(audio_err));
    }
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_RECORDING_STOP);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording stop sound skipped: %s", esp_err_to_name(sound_err));
    }
    wait_recording_upload_task();
    log_recording_diagnostics();
    vibe_audio_clear();

    show_recording_overlay("正在识别", "", true);
    char body[128];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"paste\":true}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE);
    char response[1024] = {0};
    esp_err_t err = http_request_timeout("POST", VIBE_STICK_RECORDING_STOP_PATH, body, response, sizeof(response), 30000);
    bool recording_failed = false;
    char recording_status[32] = {0};
    if (err == ESP_OK && response[0] != '\0') {
        if (parse_recording_status(response, recording_status, sizeof(recording_status))) {
            recording_failed = is_recording_failure_status(recording_status);
            if (recording_failed) {
                ESP_LOGW(TAG, "recording failed status=%s", recording_status);
            }
        }
        if (parse_state_json(response)) {
            complete_pet_fast_resume();
            render_state();
        }
    }
    if (err != ESP_OK || recording_failed || recording_upload_failed()) {
        ESP_LOGW(TAG, "recording stop bridge request failed: %s", esp_err_to_name(err));
        const char *title = (strcmp(recording_status, "audio_skipped") == 0 ||
                             strcmp(recording_status, "transcript_rejected") == 0)
            ? "未听清" : "识别失败";
        show_recording_overlay(title, "", true);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    s_recording_session_id[0] = '\0';
    s_tap_recording_active = false;
    s_motion_recording_active = false;
    poll_state();
    show_recording_overlay(NULL, NULL, false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

static void recording_finalize_task(void *arg)
{
    (void)arg;
    char event_name[sizeof(s_recording_finalize_event_name)];
    strlcpy(event_name, s_recording_finalize_event_name, sizeof(event_name));
    finish_recording_stop(event_name);
    set_recording_finalize_active(false);
    s_recording_finalize_task = NULL;
    vTaskDelete(NULL);
}

static void handle_recording_stop(const char *event_name)
{
    register_activity();
    if (recording_finalize_active()) {
        ESP_LOGI(TAG, "recording stop ignored while finalize is active");
        return;
    }
    show_recording_overlay("正在发送", "", true);
    s_tap_recording_active = false;

    if (s_recording_session_id[0] == '\0') {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        poll_state();
        show_recording_overlay(NULL, NULL, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        return;
    }

    strlcpy(s_recording_finalize_event_name, event_name, sizeof(s_recording_finalize_event_name));
    set_recording_finalize_active(true);
    BaseType_t ok = xTaskCreatePinnedToCore(recording_finalize_task, "recording_finalize", 8192,
                                            NULL, 4, &s_recording_finalize_task,
                                            VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "recording finalize task create failed; running inline");
        finish_recording_stop(event_name);
        set_recording_finalize_active(false);
        s_recording_finalize_task = NULL;
    }
}

static void handle_recording_toggle(void)
{
    register_activity();
    if (s_recording_mode != RECORDING_MODE_PUSH_TO_TALK) {
        ESP_LOGI(TAG, "front tap ignored in %s mode", recording_mode_label());
        return;
    }
    if (s_tap_recording_active || vibe_audio_is_recording() || s_recording_session_id[0] != '\0') {
        handle_recording_stop("button_tap_stop");
        s_tap_recording_active = false;
        return;
    }
    s_tap_recording_active = handle_recording_start("button_tap_start", "再按发送");
}

static bool wifi_profile_has_ssid(const wifi_profile_t *profile)
{
    return profile != NULL && profile->ssid[0] != '\0';
}

static void wifi_profile_copy(wifi_profile_t *dest, const wifi_profile_t *source)
{
    strlcpy(dest->ssid, source->ssid, sizeof(dest->ssid));
    strlcpy(dest->password, source->password, sizeof(dest->password));
}

static bool wifi_profile_merge(const wifi_profile_t *profile)
{
    if (!wifi_profile_has_ssid(profile)) {
        return false;
    }

    for (size_t i = 0; i < s_wifi_profile_count; i++) {
        if (strcmp(s_wifi_profiles[i].ssid, profile->ssid) == 0) {
            if (strcmp(s_wifi_profiles[i].password, profile->password) == 0) {
                return false;
            }
            wifi_profile_copy(&s_wifi_profiles[i], profile);
            ESP_LOGI(TAG, "updated stored Wi-Fi profile ssid=%s", profile->ssid);
            return true;
        }
    }

    if (s_wifi_profile_count >= WIFI_PROFILE_MAX_COUNT) {
        ESP_LOGW(TAG, "Wi-Fi profile store full; ignoring ssid=%s", profile->ssid);
        return false;
    }

    wifi_profile_copy(&s_wifi_profiles[s_wifi_profile_count], profile);
    s_wifi_profile_count++;
    ESP_LOGI(TAG, "added stored Wi-Fi profile ssid=%s", profile->ssid);
    return true;
}

static esp_err_t wifi_profiles_load_nvs(void)
{
    s_wifi_profile_count = 0;
    s_wifi_profile_index = 0;
    s_wifi_profile_retry_count = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_PROFILE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open Wi-Fi profile NVS");

    wifi_profile_store_t store = {0};
    size_t required_size = sizeof(store);
    err = nvs_get_blob(handle, WIFI_PROFILE_BLOB_KEY, &store, &required_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read Wi-Fi profile NVS");

    if (required_size != sizeof(store) ||
        store.magic != WIFI_PROFILE_MAGIC ||
        store.version != WIFI_PROFILE_STORE_VERSION ||
        store.count > WIFI_PROFILE_MAX_COUNT) {
        ESP_LOGW(TAG, "ignoring invalid Wi-Fi profile store");
        return ESP_OK;
    }

    for (size_t i = 0; i < store.count; i++) {
        wifi_profile_merge(&store.profiles[i]);
    }
    ESP_LOGI(TAG, "loaded %u Wi-Fi profile(s) from NVS", (unsigned)s_wifi_profile_count);
    return ESP_OK;
}

static esp_err_t wifi_profiles_save_nvs(void)
{
    wifi_profile_store_t store = {
        .magic = WIFI_PROFILE_MAGIC,
        .version = WIFI_PROFILE_STORE_VERSION,
        .count = (uint16_t)s_wifi_profile_count,
    };
    for (size_t i = 0; i < s_wifi_profile_count; i++) {
        wifi_profile_copy(&store.profiles[i], &s_wifi_profiles[i]);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open Wi-Fi profile NVS for write");
    err = nvs_set_blob(handle, WIFI_PROFILE_BLOB_KEY, &store, sizeof(store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "write Wi-Fi profile NVS");
    ESP_LOGI(TAG, "saved %u Wi-Fi profile(s) to NVS", (unsigned)s_wifi_profile_count);
    return ESP_OK;
}

static bool wifi_profiles_merge_configured(void)
{
    bool changed = false;
    const size_t configured_count =
        sizeof(k_configured_wifi_profiles) / sizeof(k_configured_wifi_profiles[0]);
    for (size_t i = 0; i < configured_count; i++) {
        changed |= wifi_profile_merge(&k_configured_wifi_profiles[i]);
    }
    return changed;
}

static bool wifi_profiles_merge_driver_config(void)
{
    wifi_config_t config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) {
        return false;
    }

    wifi_profile_t profile = {0};
    strlcpy(profile.ssid, (const char *)config.sta.ssid, sizeof(profile.ssid));
    strlcpy(profile.password, (const char *)config.sta.password, sizeof(profile.password));
    return wifi_profile_merge(&profile);
}

static esp_err_t wifi_apply_profile(size_t index)
{
    if (index >= s_wifi_profile_count) {
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_profile_t *profile = &s_wifi_profiles[index];
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, profile->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, profile->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "using Wi-Fi profile %u/%u ssid=%s",
             (unsigned)(index + 1), (unsigned)s_wifi_profile_count, profile->ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        set_wifi_connected(false);
        if (s_wifi_profile_count > 1) {
            s_wifi_profile_retry_count++;
            if (s_wifi_profile_retry_count >= WIFI_PROFILE_RETRY_LIMIT) {
                s_wifi_profile_index = (s_wifi_profile_index + 1) % s_wifi_profile_count;
                s_wifi_profile_retry_count = 0;
                ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_apply_profile(s_wifi_profile_index));
            }
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d retry=%d profile=%u/%u",
                 disconnected ? disconnected->reason : -1,
                 s_wifi_profile_retry_count,
                 (unsigned)(s_wifi_profile_index + 1),
                 (unsigned)s_wifi_profile_count);
        esp_wifi_connect();
        render_state();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        set_wifi_connected(true);
        s_wifi_profile_retry_count = 0;
        render_state();
        queue_event(VIBE_STICK_EVENT_POLL_STATE);
        queue_event(VIBE_STICK_EVENT_OTA_CHECK);
    }
}

static esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");

    ESP_RETURN_ON_ERROR(wifi_profiles_load_nvs(), TAG, "load Wi-Fi profiles");
    bool profiles_changed = wifi_profiles_merge_configured();
    profiles_changed |= wifi_profiles_merge_driver_config();
    if (profiles_changed) {
        ESP_RETURN_ON_ERROR(wifi_profiles_save_nvs(), TAG, "save Wi-Fi profiles");
    }
    if (s_wifi_profile_count == 0) {
        ESP_LOGW(TAG, "no Wi-Fi profiles configured; Wi-Fi disabled");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wifi_apply_profile(s_wifi_profile_index), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "wifi power save");
    return ESP_OK;
}

static void button_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);
}

static void button_double_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    queue_event(VIBE_STICK_EVENT_DOUBLE_CLICK);
}

static void side_button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    queue_event(VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE);
}

static void button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (s_recording_mode != RECORDING_MODE_PUSH_TO_TALK) {
        ESP_LOGI(TAG, "front long press ignored in %s mode", recording_mode_label());
        return;
    }
    if (s_tap_recording_active || vibe_audio_is_recording() || s_recording_session_id[0] != '\0') {
        ESP_LOGI(TAG, "front long press ignored while tap recording is active");
        return;
    }
    s_long_press_active = true;
    queue_event(VIBE_STICK_EVENT_LONG_START);
}

static void button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    s_wake_front_button_pending = false;
    if (s_long_press_active) {
        s_long_press_active = false;
        queue_event(VIBE_STICK_EVENT_LONG_STOP);
    }
}

static esp_err_t init_button(void)
{
    button_handle_t button = NULL;
    button_handle_t side_button = NULL;
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = VIBE_BOARD_PIN_BUTTON_FRONT,
        .active_level = 0,
        .enable_power_save = true,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &gpio_config, &button), TAG, "button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, button_single_click_cb, NULL),
                        TAG, "button single");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, button_double_click_cb, NULL),
                        TAG, "button double");
    button_event_args_t front_long_press_args = {
        .long_press = {
            .press_time = 220,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_START, &front_long_press_args, button_long_start_cb, NULL),
                        TAG, "button long");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, button_up_cb, NULL),
                        TAG, "button up");

    const button_gpio_config_t side_gpio_config = {
        .gpio_num = VIBE_BOARD_PIN_BUTTON_SIDE,
        .active_level = 0,
        .enable_power_save = false,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &side_gpio_config, &side_button), TAG, "side button");
    button_event_args_t side_long_press_args = {
        .long_press = {
            .press_time = 3000,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(side_button, BUTTON_LONG_PRESS_START, &side_long_press_args,
                                               side_button_long_start_cb, NULL),
                        TAG, "side button long");
    return ESP_OK;
}

static void capture_deep_sleep_front_button_intent(void)
{
    if (!s_woke_from_deep_sleep || s_recording_mode != RECORDING_MODE_PUSH_TO_TALK) {
        return;
    }
    if (!front_button_is_pressed()) {
        return;
    }
    s_wake_front_button_pending = true;
    ESP_LOGI(TAG, "front button held during deep sleep wake; pending PTT restore");
}

static void handle_deep_sleep_front_button_intent(void)
{
    if (!s_wake_front_button_pending) {
        return;
    }
    if (!front_button_is_pressed()) {
        s_wake_front_button_pending = false;
        ESP_LOGI(TAG, "pending deep sleep PTT restore cancelled: front button released");
        return;
    }
    if (s_recording_mode != RECORDING_MODE_PUSH_TO_TALK) {
        s_wake_front_button_pending = false;
        return;
    }
    if (s_long_press_active || s_tap_recording_active ||
        vibe_audio_is_recording() || s_recording_session_id[0] != '\0') {
        s_wake_front_button_pending = false;
        return;
    }
    if (!wifi_connected() || recording_network_busy()) {
        return;
    }
    s_wake_front_button_pending = false;
    s_long_press_active = true;
    register_activity();
    ESP_LOGI(TAG, "restoring front long press after deep sleep wake");
    queue_event(VIBE_STICK_EVENT_LONG_START);
}

static void app_task(void *arg)
{
    (void)arg;
    agent_event_t event;
    int64_t last_poll = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        update_power_saving(now_ms);
        maybe_enter_deep_sleep(now_ms);
        handle_deep_sleep_front_button_intent();
        const int state_poll_ms = s_display_power_state != DISPLAY_POWER_ACTIVE ?
            VIBE_STICK_IDLE_STATE_POLL_MS : VIBE_STICK_STATE_POLL_MS;
        const bool network_busy = recording_network_busy();
        if (wifi_connected() && !network_busy && now_ms - last_poll >= state_poll_ms) {
            last_poll = now_ms;
            poll_state();
        }
        if (vibe_motion_available() && s_recording_mode == RECORDING_MODE_LIFT_TO_TALK) {
            vibe_motion_event_t motion_event = vibe_motion_poll(now_ms);
            if (s_motion_calibrating && !vibe_motion_is_calibrating()) {
                s_motion_calibrating = false;
                s_motion_lift_armed = true;
                s_motion_start_pending = false;
                ESP_LOGI(TAG, "lift recording mode calibration complete");
                render_state();
            }
            if (!s_motion_calibrating && motion_event == VIBE_MOTION_EVENT_FLAT) {
                if (s_motion_start_pending) {
                    ESP_LOGI(TAG, "motion lift start deferred request cancelled by flat posture");
                    s_motion_start_pending = false;
                }
                if (s_motion_recording_active) {
                    (void)queue_event(VIBE_STICK_EVENT_MOTION_STOP);
                } else if (!s_motion_lift_armed) {
                    s_motion_lift_armed = true;
                }
            } else if (!s_motion_calibrating && s_motion_lift_armed &&
                       motion_event == VIBE_MOTION_EVENT_LIFTED &&
                       !s_motion_recording_active) {
                request_motion_recording_start();
            } else if (!s_motion_calibrating && s_motion_start_pending &&
                       !s_motion_recording_active) {
                request_motion_recording_start();
            }
        }
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case VIBE_STICK_EVENT_POLL_STATE:
            if (wifi_connected() && !recording_network_busy()) {
                poll_state();
            }
            break;
        case VIBE_STICK_EVENT_RECORDING_TOGGLE:
            handle_recording_toggle();
            break;
        case VIBE_STICK_EVENT_DOUBLE_CLICK:
            if (!recording_network_busy()) {
                post_simple_event("button_double", VIBE_STICK_QUOTA_REFRESH_PATH);
                poll_state();
            }
            break;
        case VIBE_STICK_EVENT_LONG_START:
            if (!handle_recording_start("button_long_start", "松开发送")) {
                s_long_press_active = false;
            }
            break;
        case VIBE_STICK_EVENT_LONG_STOP:
            handle_recording_stop("button_long_stop");
            break;
        case VIBE_STICK_EVENT_PROVIDER_NEXT:
            switch_provider();
            break;
        case VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE:
            toggle_recording_mode();
            break;
        case VIBE_STICK_EVENT_MOTION_START:
            if (s_recording_mode == RECORDING_MODE_LIFT_TO_TALK && !s_motion_recording_active) {
                s_motion_start_pending = false;
                s_motion_recording_active =
                    handle_recording_start("motion_lift_start", "放回发送");
                if (!s_motion_recording_active) {
                    s_motion_lift_armed = true;
                }
            }
            break;
        case VIBE_STICK_EVENT_MOTION_STOP:
            if (s_motion_recording_active) {
                handle_recording_stop("motion_lift_stop");
                s_motion_recording_active = false;
                s_motion_lift_armed = true;
                s_motion_start_pending = false;
            }
            break;
        case VIBE_STICK_EVENT_OTA_CHECK:
            start_ota_check_task();
            break;
        }
    }
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    uint64_t ext1_wake_status = esp_sleep_get_ext1_wakeup_status();
    s_woke_from_deep_sleep = wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED;
    if (s_woke_from_deep_sleep) {
        s_pet_fast_resume_pending = true;
        s_pet_animation_resume_ms = (esp_timer_get_time() / 1000) +
                                    VIBE_STICK_PET_FAST_RESUME_MAX_MS;
    }
    ESP_LOGI(TAG, "boot %s board=%s version=%s build=%s transport=%s",
             FIRMWARE_NAME, VIBE_BOARD_NAME, FIRMWARE_VERSION, FIRMWARE_BUILD_ID, TRANSPORT);
    ESP_LOGI(TAG, "wake cause=%d ext1_status=0x%llx",
             wake_cause, (unsigned long long)ext1_wake_status);
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_init_power());
    s_event_queue = xQueueCreate(16, sizeof(agent_event_t));
    s_lvgl_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(init_display());
    register_activity();
    lvgl_lock();
    create_ui();
    s_ui_ready = true;
    lvgl_unlock();
    render_state();
    esp_err_t motion_err = vibe_motion_init();
    if (motion_err != ESP_OK && motion_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "motion init failed: %s", esp_err_to_name(motion_err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(restore_recording_mode_preference());
    render_state();
    ESP_ERROR_CHECK(init_button());
    capture_deep_sleep_front_button_intent();
    ESP_ERROR_CHECK(vibe_audio_init());
    xTaskCreatePinnedToCore(app_task, "agent_app", 6144, NULL, 4, NULL,
                            VIBE_STICK_APP_CORE);
}
