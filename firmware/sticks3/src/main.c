#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_stick_anim_assets.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_motion.h"
#include "vibe_stick_pet_assets.h"
#include "vibe_stick_config.h"
#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_uart.h"
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
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "soc/soc_caps.h"

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
#define LVGL_TASK_STACK_BYTES 12288
#define BATTERY_FILL_MAX_WIDTH 20
#define BATTERY_LOW_THRESHOLD_PERCENT 20
#define BATTERY_HIGH_THRESHOLD_PERCENT 50
#define RECORDING_UPLOAD_BATCH_CHUNKS 4
#define RECORDING_UPLOAD_BUFFER_BYTES 8192
#define RECORDING_UPLOAD_WAIT_MS 10000
#define RECORDING_START_TIMEOUT_MS 1200
#define RECORDING_STOP_TIMEOUT_MS 210000
#define PTT_ENTER_GRACE_MS 3000
#define PTT_FOLLOWUP_REQUEST_TIMEOUT_MS 1000
#define FRONT_PTT_LONG_PRESS_MS 400
#define OTA_READ_BUFFER_BYTES 4096
#define OTA_DOWNLOAD_TIMEOUT_MS 180000
#define OTA_NO_PROGRESS_TIMEOUT_MS 20000
#define OTA_PERIODIC_CHECK_MS 300000
#define OTA_BATTERY_CHECK_MS 1800000
#define HTTP_CLIENT_BUFFER_SIZE 2048
#define BRIDGE_HEALTH_RESPONSE_BYTES 512
#define TTS_AUDIO_MAX_BYTES (1024 * 1024)
#define FIRMWARE_BUILD_ID __DATE__ " " __TIME__
#define VIBE_STICK_APP_CORE 0
#define VIBE_STICK_UI_CORE 1
#define VIBE_STICK_NETWORK_CORE 1
#define VIBE_STICK_FOLLOWUP_CORE VIBE_STICK_APP_CORE
#define VIBE_STICK_FOLLOWUP_PRIORITY 6
#define VIBE_STICK_IDLE_DIM_MS 30000
#define VIBE_STICK_IDLE_OFF_MS 60000
#define VIBE_STICK_DEEP_SLEEP_MS 300000
#define VIBE_STICK_DEEP_SLEEP_RETRY_MS 5000
#define VIBE_STICK_MOTION_WAKE_QUIET_MS 5000
#define VIBE_STICK_MOTION_WAKE_SETTLE_TIMEOUT_MS 15000
#define VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS 15000
#define VIBE_STICK_POWER_STATUS_POLL_MS 2000
#define VIBE_STICK_BATTERY_SAMPLE_COUNT 5
#define VIBE_STICK_BATTERY_FULL_LATCH_PERCENT 99
#define VIBE_STICK_BATTERY_USB_UNPLUG_HOLD_MS 30000
#define VIBE_STICK_BATTERY_WAKE_STABILIZE_MS 5000
#define VIBE_STICK_RETAINED_BOOT_MAGIC 0x56494245u
#define VIBE_STICK_BATTERY_LOG_GAP_PERCENT 5
#define VIBE_STICK_BATTERY_RTC_MAGIC 0x56424231
#define VIBE_STICK_BACKLIGHT_FADE_INTERVAL_MS 60
#define VIBE_STICK_BACKLIGHT_FADE_STEP 5
#define VIBE_STICK_PET_FAST_RESUME_MAX_MS 15000
#define VIBE_STICK_PET_ACTIVE_TIMER_MS 300
#define VIBE_STICK_PET_IDLE_TIMER_MS 1000
#define VIBE_STICK_PET_IDLE_BOB_STEPS 16
#define VIBE_STICK_MODE_SWITCH_VISUAL_MS 1800
#define VIBE_STICK_MODE_SWITCH_FRAME_MS 260
#define VIBE_STICK_CYBER_TTS_WAIT_TIMEOUT_MS 180000
#define VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS 15000
#define VIBE_STICK_MOTION_WAKE_CONFIRM_MS 500
#define VIBE_STICK_MOTION_FALSE_WAKE_DISPLAY_MS 3000
#define SIDE_MODE_TOGGLE_HOLD_MS 3000
#define SIDE_MANUAL_CALIBRATION_HOLD_MS 6000
#define VIBE_STICK_STATE_POLL_IDLE_MS 10000
#define VIBE_STICK_STATE_POLL_INTERACTIVE_MS 15000
#define VIBE_STICK_APP_IDLE_WAIT_MS 1000
#define VIBE_STICK_APP_MOTION_WAIT_MS 20
#define VIBE_STICK_WIFI_IDLE_PS WIFI_PS_MIN_MODEM
#define VIBE_STICK_WIFI_RECONNECT_MAX_MS 30000
#define VIBE_STICK_ANIM_PREVIEW 0
#ifndef VIBE_STICK_SERIAL_DEBUG_ENABLED
#define VIBE_STICK_SERIAL_DEBUG_ENABLED 0
#endif
#if VIBE_STICK_ANIM_PREVIEW
#define VIBE_STICK_OTA_ENABLED 0
#else
#define VIBE_STICK_OTA_ENABLED 1
#endif
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
#define DEVICE_PREF_RECORDING_TRIGGER_KEY "rec_trig"
#define DEVICE_PREF_RECORDING_INTENT_KEY "rec_intent"
#define DEVICE_PREF_MOTION_CALIBRATION_KEY "motion_cal_v1"
#define MOTION_CALIBRATION_MAGIC 0x564d4341u
#define MOTION_CALIBRATION_STORE_VERSION 1
#define DEEP_SLEEP_NAMESPACE "vibe_sleep"
#define DEEP_SLEEP_RECORD_KEY "last_entry"
#define DEEP_SLEEP_RECORD_MAGIC 0x56534c50u
#define DEEP_SLEEP_RECORD_VERSION 1
#define BRIDGE_TARGET_NAMESPACE "vibe_bridge"
#define BRIDGE_TARGET_HOST_KEY "host"
#define BRIDGE_TARGET_PORT_KEY "port"
#define BRIDGE_TARGET_SSID_KEY "ssid"
#define BRIDGE_TARGET_PROFILE_KEY "profile"
#define BRIDGE_TARGET_HOST_LEN 64
#define BRIDGE_TARGET_PROFILE_LEN 65
#define BRIDGE_TARGET_LABEL_LEN 65
#define BRIDGE_TARGET_TOKEN_LEN 65
#define BRIDGE_TARGET_SOURCE_LEN 12
#define BRIDGE_PROFILE_STORE_KEY "profiles"
#define BRIDGE_PROFILE_STORE_MAGIC 0x56424250u
#define BRIDGE_PROFILE_STORE_VERSION 1
#define BRIDGE_DISCOVERY_CONNECT_TIMEOUT_MS 250
#define BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE 6
#define BRIDGE_DISCOVERY_HEALTH_TIMEOUT_MS 900
#define BRIDGE_DISCOVERY_PAUSE_POLL_MS 250
#define BRIDGE_SELECTION_ENTRY_WINDOW_MS 5000
#define BRIDGE_SELECTION_CONFIRM_HOLD_MS 1500
#define BRIDGE_SELECTION_CONFIRM_ANIM_MS 1200
#define BRIDGE_SELECTION_CONFIRMED_MS 1000
#define BRIDGE_SELECTION_CLICK_SUPPRESS_MS 400

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

typedef struct {
    const char *id;
    const char *label;
    const char *host;
    int port;
    const char *token;
} bridge_profile_config_t;

#ifndef VIBE_STICK_BRIDGE_PROFILES
#define VIBE_STICK_BRIDGE_PROFILES \
    { { VIBE_STICK_BRIDGE_ID, VIBE_STICK_BRIDGE_LABEL, VIBE_STICK_BRIDGE_HOST, \
        VIBE_STICK_BRIDGE_PORT, VIBE_STICK_BRIDGE_TOKEN } }
#endif

static const bridge_profile_config_t k_configured_bridge_profiles[] = VIBE_STICK_BRIDGE_PROFILES;
_Static_assert(sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]) > 0,
               "at least one bridge profile is required");
_Static_assert(sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]) <=
                   VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT,
               "too many bridge profiles");

typedef struct {
    char id[BRIDGE_TARGET_PROFILE_LEN];
    char label[BRIDGE_TARGET_LABEL_LEN];
    char host[BRIDGE_TARGET_HOST_LEN];
    int32_t port;
    char token[BRIDGE_TARGET_TOKEN_LEN];
} bridge_discovered_profile_t;

typedef struct {
    char id[BRIDGE_TARGET_PROFILE_LEN];
    char label[BRIDGE_TARGET_LABEL_LEN];
    char host[BRIDGE_TARGET_HOST_LEN];
    int port;
    char token[BRIDGE_TARGET_TOKEN_LEN];
} bridge_profile_snapshot_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char ssid[WIFI_PROFILE_SSID_LEN];
    bridge_discovered_profile_t profiles[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT];
} bridge_profile_store_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t entry_count;
    uint32_t boot_count;
    uint64_t wake_mask;
    int64_t uptime_ms;
    int32_t battery_mv;
    int16_t battery_percent;
    uint8_t charging;
    uint8_t usb_powered;
    uint8_t recording_trigger;
    uint8_t reserved;
} deep_sleep_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char board[16];
    vibe_motion_calibration_t calibration;
} motion_calibration_store_t;

typedef enum {
    VIBE_STICK_EVENT_POLL_STATE,
    VIBE_STICK_EVENT_RECORDING_TOGGLE,
    VIBE_STICK_EVENT_DOUBLE_CLICK,
    VIBE_STICK_EVENT_LONG_START,
    VIBE_STICK_EVENT_LONG_STOP,
    VIBE_STICK_EVENT_PROVIDER_NEXT,
    VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE,
    VIBE_STICK_EVENT_RECORDING_INTENT_TOGGLE,
    VIBE_STICK_EVENT_MOTION_CALIBRATE,
    VIBE_STICK_EVENT_MOTION_START,
    VIBE_STICK_EVENT_MOTION_STOP,
    VIBE_STICK_EVENT_TTS_PROBE,
    VIBE_STICK_EVENT_OTA_CHECK,
    VIBE_STICK_EVENT_BRIDGE_SCAN_FULL,
} agent_event_type_t;

typedef struct {
    agent_event_type_t type;
} agent_event_t;

typedef enum {
    BRIDGE_CONTROL_NEXT,
    BRIDGE_CONTROL_CONFIRM,
} bridge_control_command_t;

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
    char wifi_ip[16];
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
    char host[BRIDGE_TARGET_HOST_LEN];
    int port;
    size_t profile_index;
    char profile_id[BRIDGE_TARGET_PROFILE_LEN];
    char source[BRIDGE_TARGET_SOURCE_LEN];
    char ssid[WIFI_PROFILE_SSID_LEN];
    int failure_count;
    bool available;
} bridge_target_t;

typedef struct {
    bool available;
    char board[24];
    char version[48];
    char build_id[64];
    char sha256[65];
    char elf_sha256[65];
    char url[160];
    int size;
} ota_manifest_t;

typedef enum {
    RECORDING_MODE_PUSH_TO_TALK,
    RECORDING_MODE_LIFT_TO_TALK,
    RECORDING_MODE_ANIMATION_PREVIEW,
    RECORDING_MODE_CYBER_FORTUNE,
    RECORDING_MODE_CYBER_ALMANAC,
} legacy_recording_mode_t;

typedef enum {
    RECORDING_TRIGGER_PUSH_TO_TALK,
    RECORDING_TRIGGER_LIFT_TO_TALK,
} recording_trigger_mode_t;

typedef enum {
    RECORDING_INTENT_DICTATION,
    RECORDING_INTENT_CYBER_FORTUNE,
    RECORDING_INTENT_CYBER_ALMANAC,
} recording_intent_t;

typedef enum {
    DISPLAY_POWER_ACTIVE,
    DISPLAY_POWER_DIMMED,
    DISPLAY_POWER_OFF,
} display_power_state_t;

typedef enum {
    BRIDGE_SELECTION_UI_IDLE,
    BRIDGE_SELECTION_UI_SELECTING,
    BRIDGE_SELECTION_UI_CONFIRMING,
    BRIDGE_SELECTION_UI_CONFIRMED,
} bridge_selection_ui_phase_t;

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
static QueueHandle_t s_bridge_control_queue;
static SemaphoreHandle_t s_lvgl_lock;
static atomic_bool s_wifi_connected;
static wifi_profile_t s_wifi_profiles[WIFI_PROFILE_MAX_COUNT];
static size_t s_wifi_profile_count;
static size_t s_wifi_profile_index;
static int s_wifi_profile_retry_count;
static unsigned int s_wifi_reconnect_attempt;
static esp_timer_handle_t s_wifi_reconnect_timer;
static atomic_bool s_deep_sleep_committed;
static SemaphoreHandle_t s_bridge_target_lock;
static SemaphoreHandle_t s_bridge_profiles_lock;
static SemaphoreHandle_t s_bridge_probe_lock;
static bridge_discovered_profile_t
    s_discovered_bridge_profiles[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT];
static bridge_profile_config_t
    s_discovered_bridge_profile_views[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT];
static bridge_discovered_profile_t
    s_bridge_scan_profiles[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT];
static size_t s_discovered_bridge_profile_count;
static size_t s_bridge_scan_profile_count;
static char s_bridge_profiles_loaded_ssid[WIFI_PROFILE_SSID_LEN];
static TaskHandle_t s_bridge_discovery_task;
static atomic_bool s_bridge_discovery_active;
static atomic_bool s_bridge_selection_active;
static atomic_bool s_bridge_selection_confirming;
static atomic_bool s_front_bridge_gesture_active;
static atomic_bool s_front_bridge_gesture_confirmed;
static atomic_int_fast64_t s_bridge_selection_entry_deadline_ms;
static atomic_int_fast64_t s_front_bridge_click_suppress_until_ms;
static bridge_target_t s_bridge_target = {
    .host = VIBE_STICK_BRIDGE_HOST,
    .port = VIBE_STICK_BRIDGE_PORT,
    .profile_index = 0,
    .profile_id = VIBE_STICK_BRIDGE_ID,
    .source = "boot",
    .ssid = "",
    .failure_count = 0,
    .available = false,
};
static bool s_recording_overlay_visible;
static bool s_long_press_active;
static bool s_motion_recording_active;
static bool s_motion_calibrating;
static int64_t s_motion_calibration_deadline_ms;
static bool s_motion_calibration_had_previous;
static vibe_motion_calibration_t s_motion_previous_calibration;
static bool s_motion_lift_armed;
static bool s_motion_start_pending;
static bool s_motion_wake_confirm_pending;
static int64_t s_motion_wake_confirm_deadline_ms;
static bool s_motion_wake_network_pending;
static int64_t s_motion_wake_network_deadline_ms;
static int64_t s_motion_false_wake_sleep_deadline_ms;
static bool s_tap_recording_active;
static char s_last_alert_event_id[56];
static char s_last_alert_type[24];
static bool s_alert_sound_baseline_ready;
static char s_recording_session_id[40];
static atomic_bool s_recording_session_active;
static TaskHandle_t s_recording_upload_task;
static atomic_bool s_recording_upload_active;
static atomic_bool s_recording_upload_failed;
static TaskHandle_t s_recording_finalize_task;
static atomic_bool s_recording_finalize_active;
static char s_recording_finalize_event_name[32];
static char s_ptt_followup_session_id[40];
static int64_t s_ptt_followup_enter_deadline_ms;
static atomic_bool s_ptt_followup_dispatch_active;
static char s_last_tts_playback_request_id[56];
static bool s_cyber_tts_waiting;
static int64_t s_cyber_tts_wait_deadline_ms;
static TaskHandle_t s_ota_task;
static atomic_bool s_ota_in_progress;
static int64_t s_last_ota_check_ms;
static int64_t s_last_activity_ms;
static int64_t s_last_backlight_fade_ms;
static int64_t s_last_power_status_poll_ms;
static int64_t s_next_deep_sleep_attempt_ms;
static uint8_t s_current_backlight = LCD_BACKLIGHT_DEFAULT;
static display_power_state_t s_display_power_state = DISPLAY_POWER_ACTIVE;
static recording_upload_stats_t s_recording_upload_stats;
static uint8_t *s_pet_pixels;
static vibe_stick_pet_frame_id_t s_pet_current_frame = VIBE_STICK_PET_FRAME_COUNT;
static int64_t s_pet_next_frame_ms;
static int s_pet_sequence_index;
static int s_pet_sequence_key = -1;
static int s_pet_bob_step;
static int s_pet_idle_bob_steps_remaining;
static int s_pet_y_offset;
static bool s_pet_y_offset_valid;
static lv_timer_t *s_pet_timer;
static const vibe_stick_pet_frame_id_t *s_mode_switch_frames;
static int s_mode_switch_frame_count;
static int s_mode_switch_frame_index;
static int64_t s_mode_switch_next_frame_ms;
static int64_t s_mode_switch_until_ms;
static bool s_mode_switch_persistent;
static bridge_selection_ui_phase_t s_bridge_selection_ui_phase;
static int64_t s_bridge_selection_ui_deadline_ms;
#if VIBE_STICK_ANIM_PREVIEW
static int s_anim_asset_index;
static int s_anim_frame_index;
static volatile bool s_anim_switch_requested;
static volatile bool s_anim_press_down_switch_handled;
#endif
static bool s_front_fallback_pressed;
static bool s_front_fallback_suppressed;
static int64_t s_front_fallback_down_ms;
static volatile int64_t s_front_button_iot_down_ms;
static volatile bool s_side_button_mode_hold_reached;
static volatile bool s_side_button_calibration_hold_reached;
static volatile int64_t s_front_button_iot_single_ms;
static volatile int64_t s_front_button_iot_up_ms;

static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static esp_timer_handle_t s_lvgl_tick_timer;
static TaskHandle_t s_lvgl_task;
static atomic_bool s_display_rendering_suspended;
static bool s_lvgl_tick_running;
#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
static esp_pm_lock_handle_t s_display_no_light_sleep_lock;
static bool s_display_no_light_sleep_lock_held;
#endif
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_cap;
static lv_obj_t *s_battery_bolt;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_intent_label;
static lv_obj_t *s_bridge_label;
static lv_obj_t *s_ip_label;
static lv_obj_t *s_pet_image;
static lv_obj_t *s_mode_switch_layer;
static lv_obj_t *s_mode_switch_title;
static lv_obj_t *s_mode_switch_hint;
static bool s_ui_ready;
static bool s_woke_from_deep_sleep;
static esp_sleep_wakeup_cause_t s_boot_wake_cause;
static esp_reset_reason_t s_boot_reset_reason;
static uint64_t s_boot_ext1_wake_status;
static vibe_board_boot_power_status_t s_boot_power_status;
static bool s_wake_front_button_pending;
static bool s_pet_fast_resume_pending;
static int64_t s_pet_animation_resume_ms;
static int64_t s_deep_sleep_wake_ms;
static int64_t s_external_power_removed_ms;
static int s_battery_samples[VIBE_STICK_BATTERY_SAMPLE_COUNT];
static size_t s_battery_sample_count;
static size_t s_battery_sample_index;
static bool s_battery_display_valid;
static bool s_battery_full_latched;
static int s_battery_display_level;
static int s_battery_raw_level = -1;
static int s_battery_voltage_mv = -1;
static deep_sleep_record_t s_last_deep_sleep_record;
static bool s_last_deep_sleep_record_valid;
RTC_DATA_ATTR static uint32_t s_retained_battery_magic;
RTC_DATA_ATTR static int s_retained_battery_display_level = -1;
RTC_DATA_ATTR static uint32_t s_retained_boot_magic;
RTC_DATA_ATTR static uint32_t s_retained_boot_count;
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

static void set_recording_session_active(bool active)
{
    atomic_store(&s_recording_session_active, active);
}

static void set_recording_upload_active(bool active)
{
    atomic_store(&s_recording_upload_active, active);
}

static bool recording_network_busy(void)
{
    return vibe_audio_is_recording() ||
           atomic_load(&s_recording_session_active) ||
           atomic_load(&s_recording_upload_active) ||
           recording_finalize_active();
}

static agent_state_t s_state = {
    .time = "--:--",
    .wifi = false,
    .ble = false,
    .battery = 0,
    .battery_charging = false,
    .usb_powered = false,
    .wifi_ip = "",
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
static recording_trigger_mode_t s_recording_trigger_mode = RECORDING_TRIGGER_PUSH_TO_TALK;
static recording_intent_t s_recording_intent = RECORDING_INTENT_DICTATION;

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
static bool external_powered(void);
static void request_wifi_reconnect_now(void);
static void cycle_bridge_profile(void);
static bool start_bridge_discovery_task(bool show_searching);
static void bridge_target_copy(bridge_target_t *target);
static bool bridge_target_profile_snapshot(const bridge_target_t *target,
                                           bridge_profile_snapshot_t *snapshot);
static bool bridge_profiles_merge_scan_results(const char *scan_ssid);
static bool front_button_is_pressed(void);
static void poll_front_button_fallback(int64_t now_ms);
static void update_power_saving(int64_t now_ms);
static void maybe_enter_deep_sleep(int64_t now_ms);
static void build_bridge_url(const char *path_or_url, char *url, size_t url_len);
static void clear_ptt_followup_enter_window(void);
static bool start_ptt_followup_key_dispatch(const char *event_name, agent_sound_t sound);
static void show_recording_overlay(const char *title, const char *hint, bool visible);

static bool queue_event(agent_event_type_t type)
{
    if (!s_event_queue) {
        return false;
    }
    agent_event_t event = {.type = type};
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE;
}

static bool queue_bridge_control(bridge_control_command_t command)
{
    return s_bridge_control_queue &&
           xQueueSend(s_bridge_control_queue, &command, 0) == pdTRUE;
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
    if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK && s_motion_calibrating) {
        return "CAL";
    }
    return s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK ? "LIFT" : "PTT";
}

static bool recording_intent_supported(recording_intent_t intent)
{
    if (intent == RECORDING_INTENT_CYBER_FORTUNE ||
        intent == RECORDING_INTENT_CYBER_ALMANAC) {
        return VIBE_BOARD_HAS_CYBER_INTENTS;
    }
    return true;
}

static void sanitize_recording_intent(void)
{
    if (!recording_intent_supported(s_recording_intent)) {
        s_recording_intent = RECORDING_INTENT_DICTATION;
    }
}

static const char *recording_intent_label(void)
{
    if (!recording_intent_supported(s_recording_intent)) {
        return "DICT";
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE) {
        return "FORT";
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_ALMANAC) {
        return "ALM";
    }
    return "DICT";
}

static bool recording_intent_is_cyber(void)
{
    if (!recording_intent_supported(s_recording_intent)) {
        return false;
    }
    return s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE ||
           s_recording_intent == RECORDING_INTENT_CYBER_ALMANAC;
}

static const char *recording_mode_intent(void)
{
    if (!recording_intent_supported(s_recording_intent)) {
        return "dictation";
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE) {
        return "cyber_fortune";
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_ALMANAC) {
        return "cyber_almanac";
    }
    return "dictation";
}

static void show_trigger_mode_switch_visual(void);
static void show_recording_intent_switch_visual(void);

#if VIBE_STICK_ANIM_PREVIEW
static bool recording_animation_preview_active(void)
{
    return false;
}
#endif

static void reset_recording_trigger_runtime_state(void)
{
    s_motion_recording_active = false;
    s_motion_calibrating = false;
    s_motion_calibration_deadline_ms = 0;
    s_motion_calibration_had_previous = false;
    memset(&s_motion_previous_calibration, 0, sizeof(s_motion_previous_calibration));
    s_motion_lift_armed = false;
    s_motion_start_pending = false;
    s_motion_wake_confirm_pending = false;
    s_motion_wake_confirm_deadline_ms = 0;
    s_motion_wake_network_pending = false;
    s_motion_wake_network_deadline_ms = 0;
    s_motion_false_wake_sleep_deadline_ms = 0;
}

static void set_push_to_talk_trigger_mode(void)
{
    s_recording_trigger_mode = RECORDING_TRIGGER_PUSH_TO_TALK;
    reset_recording_trigger_runtime_state();
    if (vibe_motion_available()) {
        esp_err_t err = vibe_motion_suspend();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PTT mode IMU suspend failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t load_motion_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open motion calibration NVS");

    motion_calibration_store_t store = {0};
    size_t size = sizeof(store);
    err = nvs_get_blob(handle, DEVICE_PREF_MOTION_CALIBRATION_KEY, &store, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read motion calibration");
    if (size != sizeof(store) ||
        store.magic != MOTION_CALIBRATION_MAGIC ||
        store.version != MOTION_CALIBRATION_STORE_VERSION ||
        store.size != sizeof(store) ||
        strncmp(store.board, VIBE_BOARD_NAME, sizeof(store.board)) != 0 ||
        !vibe_motion_calibration_valid(&store.calibration)) {
        ESP_LOGW(TAG, "stored motion calibration rejected board=%.*s size=%u",
                 (int)sizeof(store.board), store.board, (unsigned)size);
        if (nvs_open(DEVICE_PREF_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            esp_err_t erase_err =
                nvs_erase_key(handle, DEVICE_PREF_MOTION_CALIBRATION_KEY);
            if (erase_err == ESP_OK || erase_err == ESP_ERR_NVS_NOT_FOUND) {
                erase_err = nvs_commit(handle);
            }
            nvs_close(handle);
            if (erase_err != ESP_OK) {
                ESP_LOGW(TAG, "invalid motion calibration cleanup failed: %s",
                         esp_err_to_name(erase_err));
            }
        }
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_RETURN_ON_ERROR(vibe_motion_apply_calibration(&store.calibration),
                        TAG, "apply stored motion calibration");
    ESP_LOGI(TAG, "stored motion calibration loaded board=%s", store.board);
    return ESP_OK;
}

static esp_err_t save_motion_calibration(void)
{
    motion_calibration_store_t store = {
        .magic = MOTION_CALIBRATION_MAGIC,
        .version = MOTION_CALIBRATION_STORE_VERSION,
        .size = sizeof(motion_calibration_store_t),
    };
    snprintf(store.board, sizeof(store.board), "%s", VIBE_BOARD_NAME);
    ESP_RETURN_ON_ERROR(vibe_motion_get_calibration(&store.calibration),
                        TAG, "capture motion calibration");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open motion calibration NVS for write");
    err = nvs_set_blob(handle, DEVICE_PREF_MOTION_CALIBRATION_KEY,
                       &store, sizeof(store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "commit motion calibration");
    ESP_LOGI(TAG, "motion calibration persisted board=%s", store.board);
    return ESP_OK;
}

static esp_err_t begin_motion_calibration(const char *reason)
{
    s_motion_calibration_had_previous =
        vibe_motion_get_calibration(&s_motion_previous_calibration) == ESP_OK;
    esp_err_t err = vibe_motion_recalibrate();
    if (err != ESP_OK) {
        s_motion_calibration_had_previous = false;
        ESP_LOGW(TAG, "%s motion calibration start failed: %s",
                 reason, esp_err_to_name(err));
        return err;
    }
    s_motion_calibrating = true;
    s_motion_calibration_deadline_ms =
        esp_timer_get_time() / 1000 +
        VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS;
    s_motion_lift_armed = false;
    s_motion_start_pending = false;
    s_motion_wake_confirm_pending = false;
    s_motion_wake_confirm_deadline_ms = 0;
    s_motion_wake_network_pending = false;
    s_motion_wake_network_deadline_ms = 0;
    s_motion_false_wake_sleep_deadline_ms = 0;
    ESP_LOGI(TAG, "%s motion calibration started previous=%d",
             reason, s_motion_calibration_had_previous ? 1 : 0);
    return ESP_OK;
}

static esp_err_t set_lift_to_talk_trigger_mode(const char *reason)
{
    if (!vibe_motion_available()) {
        ESP_LOGW(TAG, "%s lift recording mode unavailable: IMU is not ready", reason);
        set_push_to_talk_trigger_mode();
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = vibe_motion_resume();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s lift recording mode IMU resume failed: %s",
                 reason, esp_err_to_name(err));
        set_push_to_talk_trigger_mode();
        return err;
    }
    s_recording_trigger_mode = RECORDING_TRIGGER_LIFT_TO_TALK;
    vibe_motion_calibration_t calibration = {0};
    if (vibe_motion_get_calibration(&calibration) != ESP_OK) {
        err = begin_motion_calibration(reason);
        if (err != ESP_OK) {
            set_push_to_talk_trigger_mode();
            return err;
        }
        return ESP_OK;
    }
    s_motion_calibrating = false;
    s_motion_calibration_deadline_ms = 0;
    s_motion_calibration_had_previous = false;
    s_motion_lift_armed = true;
    s_motion_start_pending = false;
    ESP_LOGI(TAG, "%s lift recording mode using persisted calibration", reason);
    return ESP_OK;
}

static void start_manual_motion_calibration(void)
{
    register_activity();
    if (s_recording_trigger_mode != RECORDING_TRIGGER_LIFT_TO_TALK) {
        ESP_LOGI(TAG, "manual calibration ignored outside LIFT mode");
        return;
    }
    if (s_recording_overlay_visible || vibe_audio_is_recording() ||
        recording_finalize_active()) {
        ESP_LOGI(TAG, "manual calibration ignored while recording is active");
        return;
    }
    if (begin_motion_calibration("manual") != ESP_OK) {
        return;
    }
    render_state();
}

static esp_err_t save_recording_mode_preference(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open device preference NVS for write");
    err = nvs_set_u8(handle, DEVICE_PREF_RECORDING_TRIGGER_KEY,
                     (uint8_t)s_recording_trigger_mode);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, DEVICE_PREF_RECORDING_INTENT_KEY,
                         (uint8_t)s_recording_intent);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "write recording mode preference");
    ESP_LOGI(TAG, "saved recording preference trigger=%s intent=%s",
             recording_mode_label(), recording_intent_label());
    return ESP_OK;
}

static void migrate_legacy_recording_mode(uint8_t stored_mode)
{
    switch ((legacy_recording_mode_t)stored_mode) {
    case RECORDING_MODE_LIFT_TO_TALK:
        s_recording_intent = RECORDING_INTENT_DICTATION;
        if (set_lift_to_talk_trigger_mode("stored") != ESP_OK) {
            set_push_to_talk_trigger_mode();
        }
        break;
    case RECORDING_MODE_CYBER_FORTUNE:
        s_recording_intent = RECORDING_INTENT_CYBER_FORTUNE;
        sanitize_recording_intent();
        set_push_to_talk_trigger_mode();
        break;
    case RECORDING_MODE_CYBER_ALMANAC:
        s_recording_intent = RECORDING_INTENT_CYBER_ALMANAC;
        sanitize_recording_intent();
        set_push_to_talk_trigger_mode();
        break;
    case RECORDING_MODE_PUSH_TO_TALK:
    case RECORDING_MODE_ANIMATION_PREVIEW:
    default:
        s_recording_intent = RECORDING_INTENT_DICTATION;
        set_push_to_talk_trigger_mode();
        break;
    }
}

static esp_err_t restore_recording_mode_preference(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open device preference NVS");

    uint8_t stored_trigger = 0;
    uint8_t stored_intent = 0;
    err = nvs_get_u8(handle, DEVICE_PREF_RECORDING_TRIGGER_KEY, &stored_trigger);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, DEVICE_PREF_RECORDING_INTENT_KEY, &stored_intent);
    }
    if (err == ESP_OK) {
        if (stored_intent <= (uint8_t)RECORDING_INTENT_CYBER_ALMANAC) {
            s_recording_intent = (recording_intent_t)stored_intent;
        } else {
            s_recording_intent = RECORDING_INTENT_DICTATION;
        }
        sanitize_recording_intent();
        if (stored_trigger == (uint8_t)RECORDING_TRIGGER_LIFT_TO_TALK) {
            (void)set_lift_to_talk_trigger_mode("stored");
        } else {
            set_push_to_talk_trigger_mode();
        }
        nvs_close(handle);
        ESP_LOGI(TAG, "restored recording preference trigger=%s intent=%s",
                 recording_mode_label(), recording_intent_label());
        return ESP_OK;
    }

    uint8_t stored_mode = 0;
    err = nvs_get_u8(handle, DEVICE_PREF_RECORDING_MODE_KEY, &stored_mode);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read recording mode preference");

    migrate_legacy_recording_mode(stored_mode);
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
    ESP_LOGI(TAG, "migrated recording preference trigger=%s intent=%s",
             recording_mode_label(), recording_intent_label());
    return ESP_OK;
}

static void toggle_recording_mode(void)
{
    register_activity();
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        ESP_LOGI(TAG, "recording mode switch ignored while recording");
        return;
    }
    if (s_recording_trigger_mode == RECORDING_TRIGGER_PUSH_TO_TALK) {
        if (set_lift_to_talk_trigger_mode("toggle") != ESP_OK) {
            return;
        }
    } else {
        set_push_to_talk_trigger_mode();
    }
    ESP_LOGI(TAG, "recording trigger switched to %s intent=%s",
             recording_mode_label(), recording_intent_label());
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_APPROVAL);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording mode switch sound skipped: %s", esp_err_to_name(sound_err));
    }
    render_state();
    show_trigger_mode_switch_visual();
}

static void toggle_recording_intent(void)
{
    register_activity();
    if (s_recording_overlay_visible || vibe_audio_is_recording()) {
        ESP_LOGI(TAG, "recording intent switch ignored while recording");
        return;
    }
    if (!VIBE_BOARD_HAS_CYBER_INTENTS) {
        s_recording_intent = RECORDING_INTENT_DICTATION;
        clear_ptt_followup_enter_window();
        ESP_LOGI(TAG, "recording intent switch ignored: cyber intents unavailable on %s",
                 VIBE_BOARD_NAME);
        ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
        render_state();
        show_recording_intent_switch_visual();
        return;
    }
    if (s_recording_intent == RECORDING_INTENT_DICTATION) {
        s_recording_intent = RECORDING_INTENT_CYBER_FORTUNE;
    } else if (s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE) {
        s_recording_intent = RECORDING_INTENT_CYBER_ALMANAC;
    } else {
        s_recording_intent = RECORDING_INTENT_DICTATION;
    }
    clear_ptt_followup_enter_window();
    ESP_LOGI(TAG, "recording intent switched to %s trigger=%s",
             recording_intent_label(), recording_mode_label());
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_APPROVAL);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording intent switch sound skipped: %s", esp_err_to_name(sound_err));
    }
    render_state();
    show_recording_intent_switch_visual();
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
        if (atomic_load(&s_display_rendering_suspended)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
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

#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
static bool board_requires_light_sleep_lock(void)
{
#if defined(VIBE_BOARD_STICKS3)
    return true;
#else
    return false;
#endif
}

static void update_display_light_sleep_lock(bool display_active)
{
    if (!s_display_no_light_sleep_lock) {
        return;
    }
    const bool should_hold =
        board_requires_light_sleep_lock() || display_active || external_powered();
    if (should_hold && !s_display_no_light_sleep_lock_held) {
        if (esp_pm_lock_acquire(s_display_no_light_sleep_lock) == ESP_OK) {
            s_display_no_light_sleep_lock_held = true;
        } else {
            ESP_LOGW(TAG, "could not block automatic light sleep");
        }
    } else if (!should_hold && s_display_no_light_sleep_lock_held) {
        if (esp_pm_lock_release(s_display_no_light_sleep_lock) == ESP_OK) {
            s_display_no_light_sleep_lock_held = false;
        } else {
            ESP_LOGW(TAG, "could not allow automatic light sleep");
        }
    }
}
#endif

static void set_display_rendering_suspended(bool suspended)
{
    if (!s_panel || !s_lvgl_tick_timer ||
        atomic_load(&s_display_rendering_suspended) == suspended) {
        return;
    }

#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (!suspended) {
        update_display_light_sleep_lock(true);
    }
#endif

    lvgl_lock();
    if (suspended) {
        atomic_store(&s_display_rendering_suspended, true);
        if (s_pet_timer) {
            lv_timer_pause(s_pet_timer);
        }
        if (s_lvgl_tick_running) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_lvgl_tick_timer));
            s_lvgl_tick_running = false;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, false));
    } else {
#if !VIBE_BOARD_HAS_GPIO_BACKLIGHT
        set_backlight(LCD_BACKLIGHT_DEFAULT);
#endif
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, true));
        if (!s_lvgl_tick_running) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_timer_start_periodic(s_lvgl_tick_timer,
                                         LVGL_TICK_PERIOD_MS * 1000));
            s_lvgl_tick_running = true;
        }
        if (s_pet_timer) {
            lv_timer_resume(s_pet_timer);
        }
        atomic_store(&s_display_rendering_suspended, false);
        if (s_display) {
            lv_obj_invalidate(lv_display_get_screen_active(s_display));
        }
    }
    lvgl_unlock();

#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (suspended) {
        update_display_light_sleep_lock(false);
    }
#endif

    if (!suspended && s_lvgl_task) {
        xTaskNotifyGive(s_lvgl_task);
    }
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
    const bool active_work =
           s_recording_overlay_visible ||
           vibe_audio_is_recording() ||
           s_recording_session_id[0] != '\0' ||
           s_tap_recording_active ||
           s_motion_recording_active ||
           s_motion_calibrating ||
           s_motion_wake_confirm_pending ||
           s_motion_wake_network_pending ||
           recording_finalize_active();
    return active_work;
}

static bool external_power_blocks_deep_sleep(void)
{
    return false;
}

static bool deep_sleep_should_stay_awake(void)
{
    return display_should_stay_active() ||
           external_power_blocks_deep_sleep() ||
           s_motion_start_pending ||
           ota_in_progress();
}

static bool front_button_is_pressed(void)
{
    return gpio_get_level(VIBE_BOARD_PIN_BUTTON_FRONT) == 0;
}

static void reset_front_button_fallback(void)
{
    s_front_fallback_pressed = false;
    s_front_fallback_suppressed = false;
    s_front_fallback_down_ms = 0;
}

static bool front_button_iot_handled_press(int64_t now_ms)
{
    return s_front_button_iot_down_ms > 0 &&
           now_ms - s_front_button_iot_down_ms >= 0 &&
           now_ms - s_front_button_iot_down_ms < 3000;
}

static void poll_front_button_fallback(int64_t now_ms)
{
    if (!recording_intent_is_cyber() ||
        s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK) {
        reset_front_button_fallback();
        return;
    }
    bool pressed = front_button_is_pressed();
    if (pressed && !s_front_fallback_pressed) {
        s_front_fallback_pressed = true;
        s_front_fallback_down_ms = now_ms;
        s_front_fallback_suppressed = front_button_iot_handled_press(now_ms);
        if (!s_front_fallback_suppressed) {
            ESP_LOGI(TAG, "front gpio fallback down mode=%s", recording_mode_label());
        }
        return;
    }
    if (!pressed && s_front_fallback_pressed) {
        int64_t press_ms = now_ms - s_front_fallback_down_ms;
        bool suppressed = s_front_fallback_suppressed ||
                          front_button_iot_handled_press(now_ms) ||
                          now_ms - s_front_button_iot_single_ms < 250 ||
                          now_ms - s_front_button_iot_up_ms < 250;
        reset_front_button_fallback();
        if (suppressed) {
            return;
        }
        if (press_ms < 30 || press_ms > 1500) {
            ESP_LOGI(TAG, "front gpio fallback ignored duration=%lld mode=%s",
                     (long long)press_ms,
                     recording_mode_label());
            return;
        }
        ESP_LOGI(TAG, "front gpio fallback single duration=%lld mode=%s",
                 (long long)press_ms,
                 recording_mode_label());
        queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);
    }
}

static void register_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    s_next_deep_sleep_attempt_ms = 0;
    s_motion_false_wake_sleep_deadline_ms = 0;
    request_wifi_reconnect_now();
    if (atomic_load(&s_display_rendering_suspended)) {
        set_display_rendering_suspended(false);
    }
    lvgl_lock();
    s_pet_idle_bob_steps_remaining = VIBE_STICK_PET_IDLE_BOB_STEPS;
    if (s_pet_timer) {
        lv_timer_set_period(s_pet_timer, VIBE_STICK_PET_ACTIVE_TIMER_MS);
    }
    lvgl_unlock();
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
    const bool false_wake_sleep_due =
        s_motion_false_wake_sleep_deadline_ms != 0 &&
        now_ms >= s_motion_false_wake_sleep_deadline_ms;
    if (false_wake_sleep_due) {
        next_state = DISPLAY_POWER_OFF;
        target = LCD_BACKLIGHT_OFF;
    } else if (!display_should_stay_active()) {
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
        if (false_wake_sleep_due) {
            set_backlight(target);
        } else {
            fade_backlight_toward(target, now_ms);
        }
    }
    if (next_state == DISPLAY_POWER_OFF &&
        s_current_backlight == LCD_BACKLIGHT_OFF) {
        set_display_rendering_suspended(true);
    }
    s_display_power_state = next_state;
}

static void request_motion_recording_start(void)
{
    s_motion_false_wake_sleep_deadline_ms = 0;
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
    if (!wifi_connected()) {
        if (!s_motion_wake_network_pending) {
            s_motion_wake_network_pending = true;
            s_motion_wake_network_deadline_ms =
                esp_timer_get_time() / 1000 +
                VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS;
            ESP_LOGI(TAG, "motion lift waiting up to %dms for Wi-Fi",
                     VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS);
            show_recording_overlay("CONNECTING", "", true);
            request_wifi_reconnect_now();
        }
        s_motion_start_pending = true;
        s_motion_lift_armed = false;
        return;
    }
    if (s_motion_wake_network_pending) {
        s_motion_wake_network_pending = false;
        s_motion_wake_network_deadline_ms = 0;
        ESP_LOGI(TAG, "motion lift Wi-Fi ready; starting recording");
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

static bool sleep_wake_gpio_is_active(gpio_num_t gpio)
{
    return gpio != GPIO_NUM_NC && gpio_get_level(gpio) == 0;
}

#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
static bool wait_for_motion_wake_idle(void)
{
    const int64_t started_ms = esp_timer_get_time() / 1000;
    int64_t quiet_since_ms = 0;
    ESP_LOGI(TAG, "motion wake settle start gpio=%d level=%d",
             (int)VIBE_BOARD_PIN_MOTION_WAKE,
             gpio_get_level(VIBE_BOARD_PIN_MOTION_WAKE));

    while ((esp_timer_get_time() / 1000) - started_ms <
           VIBE_STICK_MOTION_WAKE_SETTLE_TIMEOUT_MS) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool active =
            sleep_wake_gpio_is_active(VIBE_BOARD_PIN_MOTION_WAKE);
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_clear_wake_status());
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_clear_motion_wake_status());
        if (active) {
            quiet_since_ms = 0;
        } else if (quiet_since_ms == 0) {
            quiet_since_ms = now_ms;
        } else if (now_ms - quiet_since_ms >= VIBE_STICK_MOTION_WAKE_QUIET_MS) {
            ESP_LOGI(TAG, "motion wake gpio idle for %dms",
                     VIBE_STICK_MOTION_WAKE_QUIET_MS);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(TAG, "motion wake gpio did not settle within %dms",
             VIBE_STICK_MOTION_WAKE_SETTLE_TIMEOUT_MS);
    return false;
}
#endif

static uint64_t sleep_button_wake_mask(void)
{
    return 1ULL << VIBE_BOARD_PIN_BUTTON_FRONT;
}

#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
static esp_err_t configure_motion_wake_gpio_input(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << VIBE_BOARD_PIN_MOTION_WAKE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32)
static esp_err_t configure_deep_sleep_button_pullups(uint64_t wake_mask)
{
    if ((wake_mask & (1ULL << VIBE_BOARD_PIN_BUTTON_FRONT)) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_BUTTON_FRONT),
                            TAG, "front wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_BUTTON_FRONT),
                            TAG, "front wake pull-down");
    }
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if ((wake_mask & (1ULL << VIBE_BOARD_PIN_MOTION_WAKE)) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_MOTION_WAKE),
                            TAG, "motion wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_MOTION_WAKE),
                            TAG, "motion wake pull-down");
    }
#endif
    return ESP_OK;
}
#endif

static bool prepare_imu_deep_sleep_wake(uint64_t *wake_mask)
{
    if (s_recording_trigger_mode != RECORDING_TRIGGER_LIFT_TO_TALK) {
        esp_err_t err = vibe_motion_prepare_deep_sleep();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deep sleep skipped: IMU power-down failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        return true;
    }
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    esp_err_t err = vibe_motion_prepare_deep_sleep_wake();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: IMU wake prep failed: %s", esp_err_to_name(err));
        return false;
    }
    err = vibe_board_prepare_motion_wake();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: board motion wake prep failed: %s",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_resume());
        return false;
    }
    err = configure_motion_wake_gpio_input();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: motion wake gpio input failed: %s",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_cancel_motion_wake());
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_resume());
        return false;
    }
    ESP_LOGI(TAG, "motion wake gpio input enabled gpio=%d level=%d",
             (int)VIBE_BOARD_PIN_MOTION_WAKE,
             gpio_get_level(VIBE_BOARD_PIN_MOTION_WAKE));
    *wake_mask |= 1ULL << VIBE_BOARD_PIN_MOTION_WAKE;
    return true;
#else
    ESP_LOGI(TAG, "%s lift mode entering button-only deep sleep", VIBE_BOARD_NAME);
    esp_err_t err = vibe_motion_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: IMU power-down failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    return true;
#endif
}

static void cancel_imu_deep_sleep_wake(void)
{
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_cancel_motion_wake());
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_resume());
    }
#endif
}

static esp_err_t load_deep_sleep_record(void)
{
    memset(&s_last_deep_sleep_record, 0, sizeof(s_last_deep_sleep_record));
    s_last_deep_sleep_record_valid = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEEP_SLEEP_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open deep sleep record");

    size_t size = sizeof(s_last_deep_sleep_record);
    err = nvs_get_blob(handle, DEEP_SLEEP_RECORD_KEY,
                       &s_last_deep_sleep_record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read deep sleep record");
    if (size != sizeof(s_last_deep_sleep_record) ||
        s_last_deep_sleep_record.magic != DEEP_SLEEP_RECORD_MAGIC ||
        s_last_deep_sleep_record.version != DEEP_SLEEP_RECORD_VERSION ||
        s_last_deep_sleep_record.size != sizeof(s_last_deep_sleep_record)) {
        ESP_LOGW(TAG, "ignored incompatible deep sleep record size=%u",
                 (unsigned)size);
        memset(&s_last_deep_sleep_record, 0, sizeof(s_last_deep_sleep_record));
        return ESP_OK;
    }

    s_last_deep_sleep_record_valid = true;
    ESP_LOGI(TAG,
             "deep sleep record entries=%lu boot=%lu uptime_ms=%lld wake_mask=0x%llx battery=%dmV/%d%% charging=%u usb=%u trigger=%u",
             (unsigned long)s_last_deep_sleep_record.entry_count,
             (unsigned long)s_last_deep_sleep_record.boot_count,
             (long long)s_last_deep_sleep_record.uptime_ms,
             (unsigned long long)s_last_deep_sleep_record.wake_mask,
             (int)s_last_deep_sleep_record.battery_mv,
             (int)s_last_deep_sleep_record.battery_percent,
             (unsigned)s_last_deep_sleep_record.charging,
             (unsigned)s_last_deep_sleep_record.usb_powered,
             (unsigned)s_last_deep_sleep_record.recording_trigger);
    return ESP_OK;
}

static esp_err_t save_deep_sleep_record(uint64_t wake_mask)
{
    deep_sleep_record_t record = {
        .magic = DEEP_SLEEP_RECORD_MAGIC,
        .version = DEEP_SLEEP_RECORD_VERSION,
        .size = sizeof(deep_sleep_record_t),
        .entry_count = s_last_deep_sleep_record_valid
                           ? s_last_deep_sleep_record.entry_count + 1
                           : 1,
        .boot_count = s_retained_boot_count,
        .wake_mask = wake_mask,
        .uptime_ms = esp_timer_get_time() / 1000,
        .battery_mv = s_battery_voltage_mv,
        .battery_percent = s_battery_raw_level,
        .charging = s_state.battery_charging ? 1 : 0,
        .usb_powered = s_state.usb_powered ? 1 : 0,
        .recording_trigger = (uint8_t)s_recording_trigger_mode,
    };

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEEP_SLEEP_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open deep sleep record for write");
    err = nvs_set_blob(handle, DEEP_SLEEP_RECORD_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "commit deep sleep record");

    s_last_deep_sleep_record = record;
    s_last_deep_sleep_record_valid = true;
    ESP_LOGI(TAG, "deep sleep entry persisted count=%lu",
             (unsigned long)record.entry_count);
    return ESP_OK;
}

static bool enter_deep_sleep(void)
{
    uint64_t wake_mask = sleep_button_wake_mask();
    gpio_num_t ext0_gpio = VIBE_BOARD_PIN_BUTTON_FRONT;
    if (sleep_wake_gpio_is_active(ext0_gpio)) {
        ESP_LOGW(TAG, "deep sleep skipped: ext0 wake gpio=%d is already active",
                 (int)ext0_gpio);
        return false;
    }

    if (!prepare_imu_deep_sleep_wake(&wake_mask)) {
        return false;
    }
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (!wait_for_motion_wake_idle()) {
        cancel_imu_deep_sleep_wake();
        return false;
    }
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32)
    esp_err_t pull_err = configure_deep_sleep_button_pullups(wake_mask);
    if (pull_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: wake pull-up setup failed: %s",
                 esp_err_to_name(pull_err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
#endif
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    const bool motion_wake_enabled =
        s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK &&
        VIBE_BOARD_PIN_MOTION_WAKE != GPIO_NUM_NC;
    if (motion_wake_enabled &&
        sleep_wake_gpio_is_active(VIBE_BOARD_PIN_MOTION_WAKE)) {
        ESP_LOGW(TAG, "deep sleep skipped: motion wake gpio=%d is already active",
                 (int)VIBE_BOARD_PIN_MOTION_WAKE);
        cancel_imu_deep_sleep_wake();
        return false;
    }
#else
    const bool motion_wake_enabled = false;
#endif

    esp_err_t err = save_deep_sleep_record(wake_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: persistent record failed: %s",
                 esp_err_to_name(err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    err = vibe_audio_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep skipped: audio power-down failed: %s",
                 esp_err_to_name(err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    set_display_rendering_suspended(true);
    set_backlight(LCD_BACKLIGHT_OFF);

    ESP_LOGI(TAG, "entering deep sleep board=%s mode=%s wake_mask=0x%llx",
             VIBE_BOARD_NAME, recording_mode_label(), (unsigned long long)wake_mask);
    atomic_store(&s_deep_sleep_committed, true);
    if (s_wifi_reconnect_timer &&
        esp_timer_is_active(s_wifi_reconnect_timer)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_wifi_reconnect_timer));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(50));
    err = vibe_board_prepare_deep_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "board peripheral power-down incomplete; entering deep sleep: %s",
                 esp_err_to_name(err));
    }

    err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "final wake reset failed; restarting instead of sleeping: %s",
                 esp_err_to_name(err));
        esp_restart();
    }
    err = esp_sleep_enable_ext0_wakeup(ext0_gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "final wake source setup failed; restarting instead of sleeping: %s",
                 esp_err_to_name(err));
        esp_restart();
    }
    if (motion_wake_enabled) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        err = esp_sleep_enable_ext1_wakeup_io(
            1ULL << VIBE_BOARD_PIN_MOTION_WAKE,
            ESP_EXT1_WAKEUP_ALL_LOW);
#else
        err = esp_sleep_enable_ext1_wakeup_io(
            1ULL << VIBE_BOARD_PIN_MOTION_WAKE,
            ESP_EXT1_WAKEUP_ANY_LOW);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "motion wake source setup failed; restarting instead of sleeping: %s",
                     esp_err_to_name(err));
            esp_restart();
        }
    }
    esp_deep_sleep_start();
    return true;
}

static void maybe_enter_deep_sleep(int64_t now_ms)
{
    const bool false_wake_sleep_due =
        s_motion_false_wake_sleep_deadline_ms != 0 &&
        now_ms >= s_motion_false_wake_sleep_deadline_ms;
    if (s_last_activity_ms == 0 ||
        deep_sleep_should_stay_awake() ||
        s_current_backlight != LCD_BACKLIGHT_OFF) {
        return;
    }
    if (!false_wake_sleep_due &&
        (now_ms - s_last_activity_ms) < VIBE_STICK_DEEP_SLEEP_MS) {
        return;
    }
    if (s_next_deep_sleep_attempt_ms != 0 &&
        now_ms < s_next_deep_sleep_attempt_ms) {
        return;
    }
    if (!enter_deep_sleep()) {
        s_next_deep_sleep_attempt_ms =
            now_ms + VIBE_STICK_DEEP_SLEEP_RETRY_MS;
    }
}

static const char *wake_cause_label(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
        return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
        return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        return "touch";
    case ESP_SLEEP_WAKEUP_ULP:
        return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
        return "uart";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "undefined";
    default:
        return "other";
    }
}

static const char *reset_reason_label(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

static void init_backlight(void)
{
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK,
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

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = VIBE_BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP), TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(s_display, s_panel);
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
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s_lvgl_tick_timer), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(s_lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000),
        TAG, "tick start");
    s_lvgl_tick_running = true;

    BaseType_t task_ok =
        xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK_BYTES, NULL, 3,
                                &s_lvgl_task, VIBE_STICK_UI_CORE);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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

    int fill_width = battery_value > 0 ? (battery_value * BATTERY_FILL_MAX_WIDTH) / 100 : 0;
    if (fill_width < 1 && battery_value > 0) {
        fill_width = 1;
    }

    const bool external_power = charging || usb_powered;
    lv_color_t battery_color = lv_color_hex(0x6b7280);
    if (s_battery_display_valid) {
        if (battery_value < BATTERY_LOW_THRESHOLD_PERCENT) {
            battery_color = lv_color_hex(0xef4444);
        } else if (battery_value < BATTERY_HIGH_THRESHOLD_PERCENT) {
            battery_color = lv_color_hex(0xfacc15);
        } else {
            battery_color = lv_color_hex(0x32d583);
        }
    }

    lv_obj_set_style_border_color(s_battery_icon, battery_color, 0);
    lv_obj_set_style_bg_color(s_battery_fill, battery_color, 0);
    lv_obj_set_style_bg_color(s_battery_cap, battery_color, 0);
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

static const vibe_stick_pet_frame_id_t s_mode_switch_ptt_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_ATTENTION,
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_MINI_TYPING,
};

static const vibe_stick_pet_frame_id_t s_mode_switch_lift_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_CARRYING,
    VIBE_STICK_PET_FRAME_CLOUDLING_REACT_DRAG,
    VIBE_STICK_PET_FRAME_CLOUDLING_CARRYING,
};

static const vibe_stick_pet_frame_id_t s_mode_switch_dict_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_MINI_TYPING,
    VIBE_STICK_PET_FRAME_CLOUDLING_TYPING,
};

static const vibe_stick_pet_frame_id_t s_mode_switch_fortune_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
    VIBE_STICK_PET_FRAME_CLOUDLING_JUGGLING,
    VIBE_STICK_PET_FRAME_CLOUDLING_THINKING,
};

static const vibe_stick_pet_frame_id_t s_mode_switch_almanac_frames[] = {
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_READING,
    VIBE_STICK_PET_FRAME_CLOUDLING_NOTIFICATION,
    VIBE_STICK_PET_FRAME_CLOUDLING_IDLE_READING,
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

static void set_pet_vertical_offset(int y_offset)
{
    if (!s_pet_image ||
        (s_pet_y_offset_valid && s_pet_y_offset == y_offset)) {
        return;
    }
    lv_obj_align(s_pet_image, LV_ALIGN_CENTER, 0, y_offset);
    s_pet_y_offset = y_offset;
    s_pet_y_offset_valid = true;
}

static void set_pet_timer_period(uint32_t period_ms)
{
    if (s_pet_timer) {
        lv_timer_set_period(s_pet_timer, period_ms);
    }
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

#if VIBE_STICK_ANIM_PREVIEW
static void switch_anim_preview_asset(void);
#endif
static void update_pet_visual(void);

static void complete_pet_fast_resume(void)
{
    s_pet_fast_resume_pending = false;
    s_woke_from_deep_sleep = false;
}

static bool mode_switch_visual_active(int64_t now_ms)
{
    return (s_mode_switch_persistent || s_mode_switch_until_ms > now_ms) &&
           s_mode_switch_frames != NULL &&
           s_mode_switch_frame_count > 0;
}

static void finish_mode_switch_visual(void)
{
    if (s_mode_switch_layer) {
        lv_obj_add_flag(s_mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    }
    s_mode_switch_until_ms = 0;
    s_mode_switch_frames = NULL;
    s_mode_switch_frame_count = 0;
    s_mode_switch_frame_index = 0;
    s_mode_switch_next_frame_ms = 0;
    s_mode_switch_persistent = false;
    s_pet_sequence_key = -1;
    s_pet_next_frame_ms = 0;
}

static void show_mode_switch_visual_internal(const char *title, const char *hint,
                                             const vibe_stick_pet_frame_id_t *frames,
                                             int frame_count, lv_color_t accent,
                                             bool persistent)
{
    if (!s_ui_ready || !s_mode_switch_layer || !title || !hint || !frames || frame_count <= 0) {
        return;
    }
    lvgl_lock();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_mode_switch_frames = frames;
    s_mode_switch_frame_count = frame_count;
    s_mode_switch_frame_index = 0;
    s_mode_switch_next_frame_ms = 0;
    s_mode_switch_persistent = persistent;
    s_mode_switch_until_ms =
        persistent ? 0 : now_ms + VIBE_STICK_MODE_SWITCH_VISUAL_MS;
    lv_label_set_text(s_mode_switch_title, title);
    lv_label_set_text(s_mode_switch_hint, hint);
    lv_obj_set_style_text_color(s_mode_switch_title, accent, 0);
    lv_obj_clear_flag(s_mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    update_pet_visual();
    lvgl_unlock();
    ESP_LOGI(TAG, "mode visual title=%s hint=%s persistent=%d",
             title, hint, persistent);
}

static void show_mode_switch_visual(const char *title, const char *hint,
                                    const vibe_stick_pet_frame_id_t *frames,
                                    int frame_count, lv_color_t accent)
{
    show_mode_switch_visual_internal(title, hint, frames, frame_count, accent, false);
}

static void show_persistent_mode_switch_visual(const char *title, const char *hint,
                                               const vibe_stick_pet_frame_id_t *frames,
                                               int frame_count, lv_color_t accent)
{
    show_mode_switch_visual_internal(title, hint, frames, frame_count, accent, true);
}

static void bridge_selection_title(char *title, size_t title_len)
{
    bridge_target_t target;
    bridge_profile_snapshot_t profile;
    bridge_target_copy(&target);
    if (bridge_target_profile_snapshot(&target, &profile)) {
        strlcpy(title, profile.label[0] != '\0' ? profile.label : profile.id,
                title_len);
        return;
    }
    strlcpy(title, "NO SAVED BRIDGE", title_len);
}

static void show_bridge_selection_visual(const char *status, lv_color_t accent)
{
    char title[BRIDGE_TARGET_LABEL_LEN] = {0};
    bridge_selection_title(title, sizeof(title));
    show_persistent_mode_switch_visual(
        title, status,
        s_mode_switch_dict_frames,
        sizeof(s_mode_switch_dict_frames) / sizeof(s_mode_switch_dict_frames[0]),
        accent);
    s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_SELECTING;
    s_bridge_selection_ui_deadline_ms = 0;
}

static void refresh_bridge_selection_visual(void)
{
    if (!atomic_load(&s_bridge_selection_active) ||
        atomic_load(&s_bridge_selection_confirming) ||
        s_bridge_selection_ui_phase != BRIDGE_SELECTION_UI_SELECTING) {
        return;
    }
    bridge_target_t target;
    bridge_target_copy(&target);
    show_bridge_selection_visual(target.available ? "ONLINE" : "OFFLINE",
                                 target.available ? lv_color_hex(0x86efac)
                                                  : lv_color_hex(0xfca5a5));
}

static void begin_bridge_selection_confirmation(void)
{
    char title[BRIDGE_TARGET_LABEL_LEN] = {0};
    bridge_selection_title(title, sizeof(title));
    show_persistent_mode_switch_visual(
        title, "CONFIRMING",
        s_pet_approval_frames,
        sizeof(s_pet_approval_frames) / sizeof(s_pet_approval_frames[0]),
        lv_color_hex(0x93c5fd));
    s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_CONFIRMING;
    s_bridge_selection_ui_deadline_ms =
        esp_timer_get_time() / 1000 + BRIDGE_SELECTION_CONFIRM_ANIM_MS;
}

static void show_trigger_mode_switch_visual(void)
{
    const bool lift = s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK;
    const agent_provider_config_t *provider = current_provider_config();
    show_mode_switch_visual(lift ? "LIFT TO TALK" : "PUSH TO TALK",
                            lift ? "SIDE 3S  LIFT" : "SIDE 3S  PTT",
                            lift ? s_mode_switch_lift_frames : s_mode_switch_ptt_frames,
                            lift ? (int)(sizeof(s_mode_switch_lift_frames) /
                                         sizeof(s_mode_switch_lift_frames[0]))
                                 : (int)(sizeof(s_mode_switch_ptt_frames) /
                                         sizeof(s_mode_switch_ptt_frames[0])),
                            provider->accent_color);
}

static void show_recording_intent_switch_visual(void)
{
    const agent_provider_config_t *provider = current_provider_config();
    if (s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE) {
        show_mode_switch_visual("FORTUNE",
                                "SIDE 2X  FORT",
                                s_mode_switch_fortune_frames,
                                sizeof(s_mode_switch_fortune_frames) /
                                    sizeof(s_mode_switch_fortune_frames[0]),
                                provider->accent_color);
        return;
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_ALMANAC) {
        show_mode_switch_visual("ALMANAC",
                                "SIDE 2X  ALM",
                                s_mode_switch_almanac_frames,
                                sizeof(s_mode_switch_almanac_frames) /
                                    sizeof(s_mode_switch_almanac_frames[0]),
                                provider->accent_color);
        return;
    }
    show_mode_switch_visual("DICTATION",
                            "SIDE 2X  DICT",
                            s_mode_switch_dict_frames,
                            sizeof(s_mode_switch_dict_frames) /
                                sizeof(s_mode_switch_dict_frames[0]),
                            lv_color_hex(0xf4f5f7));
}

static void update_pet_visual(void)
{
    if (s_display_power_state != DISPLAY_POWER_ACTIVE && !s_recording_overlay_visible) {
        return;
    }
    if (!s_pet_image) {
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_bridge_selection_ui_phase == BRIDGE_SELECTION_UI_CONFIRMING &&
        now_ms >= s_bridge_selection_ui_deadline_ms) {
        s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_CONFIRMED;
        s_bridge_selection_ui_deadline_ms =
            now_ms + BRIDGE_SELECTION_CONFIRMED_MS;
        s_mode_switch_frames = s_pet_done_frames;
        s_mode_switch_frame_count =
            sizeof(s_pet_done_frames) / sizeof(s_pet_done_frames[0]);
        s_mode_switch_frame_index = 0;
        s_mode_switch_next_frame_ms = 0;
        lv_label_set_text(s_mode_switch_hint, "CONFIRMED");
        lv_obj_set_style_text_color(s_mode_switch_title,
                                    lv_color_hex(0x86efac), 0);
        ESP_LOGI(TAG, "bridge selection confirmation visible");
    } else if (s_bridge_selection_ui_phase == BRIDGE_SELECTION_UI_CONFIRMED &&
               now_ms >= s_bridge_selection_ui_deadline_ms) {
        s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_IDLE;
        s_bridge_selection_ui_deadline_ms = 0;
        atomic_store(&s_bridge_selection_active, false);
        atomic_store(&s_bridge_selection_confirming, false);
        atomic_store(&s_front_bridge_gesture_active, false);
        atomic_store(&s_front_bridge_gesture_confirmed, false);
        finish_mode_switch_visual();
        ESP_LOGI(TAG, "bridge selection mode exited");
    }
    if (mode_switch_visual_active(now_ms)) {
        if (now_ms >= s_mode_switch_next_frame_ms) {
            set_pet_frame(s_mode_switch_frames[s_mode_switch_frame_index]);
            s_mode_switch_frame_index =
                (s_mode_switch_frame_index + 1) % s_mode_switch_frame_count;
            s_mode_switch_next_frame_ms = now_ms + VIBE_STICK_MODE_SWITCH_FRAME_MS;
        }
        set_pet_timer_period(VIBE_STICK_PET_ACTIVE_TIMER_MS);
        set_pet_vertical_offset(24);
        return;
    }
    if (s_mode_switch_until_ms != 0) {
        finish_mode_switch_visual();
    }
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        if (s_anim_switch_requested) {
            s_anim_switch_requested = false;
            switch_anim_preview_asset();
        }
        if (now_ms >= s_pet_next_frame_ms) {
            int frame_count = vibe_stick_anim_frame_count(s_anim_asset_index);
            if (frame_count <= 0) {
                return;
            }
            if (vibe_stick_anim_decode_frame(s_anim_asset_index,
                                             s_anim_frame_index,
                                             s_pet_pixels,
                                             VIBE_STICK_ANIM_PIXEL_BYTES)) {
                lv_obj_invalidate(s_pet_image);
            } else {
                ESP_LOGW(TAG, "anim frame decode failed asset=%d frame=%d",
                         s_anim_asset_index, s_anim_frame_index);
            }
            s_anim_frame_index = (s_anim_frame_index + 1) % frame_count;
            s_pet_next_frame_ms = now_ms + 1000 / vibe_stick_anim_fps();
        }
        set_pet_timer_period(VIBE_STICK_PET_ACTIVE_TIMER_MS);
        set_pet_vertical_offset(14);
        return;
    }
#endif

    const int bob_offsets[] = {0, -2, -4, -2, 0, 2, 4, 2};
    const provider_display_state_t *display_state = current_provider_display_state();
    const agent_provider_config_t *provider = current_provider_config();
    const char *status = provider->implemented ? display_state->status : "UNIMPLEMENTED";
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
        if (sequence.key == 0) {
            s_pet_idle_bob_steps_remaining = VIBE_STICK_PET_IDLE_BOB_STEPS;
        }
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
    if (sequence.key != 0 || s_pet_idle_bob_steps_remaining > 0) {
        set_pet_timer_period(VIBE_STICK_PET_ACTIVE_TIMER_MS);
        set_pet_vertical_offset(14 + bob_offsets[s_pet_bob_step]);
        s_pet_bob_step = (s_pet_bob_step + 1) %
                         (int)(sizeof(bob_offsets) / sizeof(bob_offsets[0]));
        if (sequence.key == 0) {
            s_pet_idle_bob_steps_remaining--;
        }
    } else {
        set_pet_timer_period(VIBE_STICK_PET_IDLE_TIMER_MS);
        set_pet_vertical_offset(14);
    }
}

static void pet_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_pet_visual();
}

#if VIBE_STICK_ANIM_PREVIEW
static void switch_anim_preview_asset(void)
{
    int asset_count = vibe_stick_anim_asset_count();
    if (asset_count <= 0 || !s_pet_pixels || !s_pet_image) {
        return;
    }
    s_anim_asset_index = (s_anim_asset_index + 1) % asset_count;
    s_anim_frame_index = 0;
    s_pet_next_frame_ms = 0;
    if (vibe_stick_anim_decode_frame(s_anim_asset_index,
                                     s_anim_frame_index,
                                     s_pet_pixels,
                                     VIBE_STICK_ANIM_PIXEL_BYTES)) {
        s_anim_frame_index = 1 % vibe_stick_anim_frame_count(s_anim_asset_index);
        lv_obj_clear_flag(s_pet_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_pet_image);
        ESP_LOGI(TAG, "anim preview asset=%d/%d name=%s",
                 s_anim_asset_index + 1,
                 asset_count,
                 vibe_stick_anim_asset_name(s_anim_asset_index));
    } else {
        ESP_LOGW(TAG, "anim preview switch decode failed asset=%d", s_anim_asset_index);
    }
}
#endif

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

    s_battery_icon = make_plain_obj(screen, 26, 13, lv_color_hex(0x000000), LV_OPA_TRANSP, 3);
    lv_obj_set_style_border_width(s_battery_icon, 1, 0);
    lv_obj_set_style_border_color(s_battery_icon, lv_color_hex(0xf3f4f6), 0);
    lv_obj_align(s_battery_icon, LV_ALIGN_TOP_RIGHT, -9, 9);
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

    s_mode_label = make_label(screen, "PTT", &lv_font_montserrat_10, lv_color_hex(0x8a9099), 28, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_MID, -18, 9);
    s_intent_label = make_label(screen, "DICT", &lv_font_montserrat_10, lv_color_hex(0x8a9099), 32, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(s_intent_label, LV_ALIGN_TOP_MID, 12, 9);

    s_bridge_label = make_label(screen, "B CapsWriter", &lv_font_montserrat_10,
                                 lv_color_hex(0x686e78), 128, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_bridge_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_bridge_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    s_ip_label = make_label(screen, "IP --", &lv_font_montserrat_10, lv_color_hex(0x686e78), 128, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_ip_label, LV_ALIGN_BOTTOM_MID, 0, -7);

    s_pet_image = lv_image_create(screen);
#if VIBE_STICK_ANIM_PREVIEW
    s_pet_pixels = heap_caps_malloc(VIBE_STICK_ANIM_PIXEL_BYTES, MALLOC_CAP_8BIT);
    if (s_pet_pixels && vibe_stick_anim_assets_init()) {
        vibe_stick_anim_set_image_data(s_pet_pixels);
        if (vibe_stick_anim_decode_frame(0, 0, s_pet_pixels, VIBE_STICK_ANIM_PIXEL_BYTES)) {
            lv_image_set_src(s_pet_image, &vibe_stick_anim_image);
            s_anim_asset_index = 0;
            s_anim_frame_index = 1;
            s_pet_next_frame_ms = 0;
            ESP_LOGI(TAG, "anim preview asset=%d/%d name=%s",
                     s_anim_asset_index + 1,
                     vibe_stick_anim_asset_count(),
                     vibe_stick_anim_asset_name(s_anim_asset_index));
        } else {
            lv_obj_add_flag(s_pet_image, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        ESP_LOGW(TAG, "anim preview initialization failed");
        lv_obj_add_flag(s_pet_image, LV_OBJ_FLAG_HIDDEN);
    }
#else
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
#endif
    set_pet_vertical_offset(14);
#if VIBE_STICK_ANIM_PREVIEW
    s_pet_timer = lv_timer_create(pet_timer_cb, 1000 / vibe_stick_anim_fps(), NULL);
#else
    s_pet_timer = lv_timer_create(pet_timer_cb, VIBE_STICK_PET_ACTIVE_TIMER_MS, NULL);
#endif

    s_mode_switch_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_mode_switch_layer);
    lv_obj_set_size(s_mode_switch_layer, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_mode_switch_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_mode_switch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mode_switch_layer, LV_OBJ_FLAG_HIDDEN);
    s_mode_switch_title = make_label(s_mode_switch_layer, "DICTATION",
                                     &lv_font_montserrat_20,
                                     lv_color_hex(0xf4f5f7), 124,
                                     LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_mode_switch_title, LV_ALIGN_CENTER, 0, -50);
    s_mode_switch_hint = make_label(s_mode_switch_layer, "SIDE 2X  DICT",
                                    &lv_font_montserrat_14,
                                    lv_color_hex(0x9aa1ad), 124,
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(s_mode_switch_hint, LV_ALIGN_CENTER, 0, -26);

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

    s_recording_title = make_label(s_recording_overlay, "LISTENING", FONT_ASCII,
                                   lv_color_hex(0xf4f5f7), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_recording_title, FONT_ASCII, 0);
    lv_obj_align(s_recording_title, LV_ALIGN_CENTER, 0, 22);
    s_recording_hint = make_label(s_recording_overlay, "RELEASE TO SEND", FONT_ASCII,
                                  lv_color_hex(0x8b9098), 120, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_recording_hint, FONT_ASCII, 0);
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
    char ip_text[24];
    snprintf(ip_text, sizeof(ip_text), "IP %s",
             connected && s_state.wifi_ip[0] != '\0' ? s_state.wifi_ip : "--");
    lv_label_set_text(s_ip_label, ip_text);
    lv_obj_set_style_text_color(s_ip_label,
                                connected ? lv_color_hex(0x8a9099) : lv_color_hex(0x4b515a),
                                0);
    set_battery_ui(s_state.battery, s_state.battery_charging, s_state.usb_powered);
    lv_label_set_text(s_mode_label, recording_mode_label());
    lv_obj_set_style_text_color(s_mode_label,
                                s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK ?
                                    provider->accent_color : lv_color_hex(0x8a9099),
                                0);
    lv_label_set_text(s_intent_label, recording_intent_label());
    lv_obj_set_style_text_color(s_intent_label,
                                recording_intent_is_cyber() ?
                                    provider->accent_color : lv_color_hex(0x8a9099),
                                0);
    bridge_target_t target;
    bridge_target_copy(&target);
    bridge_profile_snapshot_t bridge_profile;
    bool bridge_profile_valid = bridge_target_profile_snapshot(&target, &bridge_profile);
    char bridge_text[40];
    snprintf(bridge_text, sizeof(bridge_text), "B %.37s",
             bridge_profile_valid && bridge_profile.label[0] != '\0'
                 ? bridge_profile.label
                 : "Unassigned");
    lv_label_set_text(s_bridge_label, bridge_text);
    lv_obj_set_style_text_color(s_bridge_label,
                                target.available ? lv_color_hex(0x8a9099) : lv_color_hex(0xc98484),
                                0);
    update_pet_visual();
    lvgl_unlock();
}

static void show_recording_overlay(const char *title, const char *hint, bool visible)
{
    lvgl_lock();
    if (visible) {
        if (title) {
            lv_obj_set_style_text_font(s_recording_title, FONT_ASCII, 0);
            lv_label_set_text(s_recording_title, title);
        }
        if (hint) {
            lv_obj_set_style_text_font(s_recording_hint, FONT_ASCII, 0);
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

static void clear_cyber_tts_wait(void)
{
    s_cyber_tts_waiting = false;
    s_cyber_tts_wait_deadline_ms = 0;
}

static void start_cyber_tts_wait(void)
{
    s_cyber_tts_waiting = true;
    s_cyber_tts_wait_deadline_ms =
        esp_timer_get_time() / 1000 + VIBE_STICK_CYBER_TTS_WAIT_TIMEOUT_MS;
    ESP_LOGI(TAG, "cyber tts wait started");
    show_recording_overlay("SENDING", "", true);
}

static void maybe_timeout_cyber_tts_wait(int64_t now_ms)
{
    if (!s_cyber_tts_waiting || now_ms < s_cyber_tts_wait_deadline_ms) {
        return;
    }
    ESP_LOGW(TAG, "cyber tts wait timed out");
    clear_cyber_tts_wait();
    show_recording_overlay(NULL, NULL, false);
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

static int current_wifi_rssi(void);

static void current_wifi_ssid(char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0) {
        return;
    }
    ssid[0] = '\0';
    wifi_ap_record_t ap = {0};
    if (wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ssid, (const char *)ap.ssid, ssid_len);
    }
}

static void current_wifi_bssid(char *bssid, size_t bssid_len)
{
    if (!bssid || bssid_len == 0) {
        return;
    }
    bssid[0] = '\0';
    wifi_ap_record_t ap = {0};
    if (wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(bssid, bssid_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
}

static void device_id(char *id, size_t id_len)
{
    if (!id || id_len == 0) {
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(id, id_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strlcpy(id, VIBE_BOARD_NAME, id_len);
    }
}

static void bridge_target_copy(bridge_target_t *target)
{
    if (!target) {
        return;
    }
    if (s_bridge_target_lock) {
        xSemaphoreTake(s_bridge_target_lock, portMAX_DELAY);
    }
    *target = s_bridge_target;
    if (s_bridge_target_lock) {
        xSemaphoreGive(s_bridge_target_lock);
    }
}

static void bridge_profiles_lock(void)
{
    if (s_bridge_profiles_lock) {
        xSemaphoreTake(s_bridge_profiles_lock, portMAX_DELAY);
    }
}

static void bridge_profiles_unlock(void)
{
    if (s_bridge_profiles_lock) {
        xSemaphoreGive(s_bridge_profiles_lock);
    }
}

static void bridge_probe_lock(void)
{
    if (s_bridge_probe_lock) {
        xSemaphoreTake(s_bridge_probe_lock, portMAX_DELAY);
    }
}

static void bridge_probe_unlock(void)
{
    if (s_bridge_probe_lock) {
        xSemaphoreGive(s_bridge_probe_lock);
    }
}

static void bridge_profile_snapshot_from_config(const bridge_profile_config_t *profile,
                                                bridge_profile_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (!profile) {
        return;
    }
    strlcpy(snapshot->id, profile->id ? profile->id : "", sizeof(snapshot->id));
    strlcpy(snapshot->label, profile->label ? profile->label : "", sizeof(snapshot->label));
    strlcpy(snapshot->host, profile->host ? profile->host : "", sizeof(snapshot->host));
    snapshot->port = profile->port;
    strlcpy(snapshot->token, profile->token ? profile->token : "", sizeof(snapshot->token));
}

static void bridge_profile_snapshot_from_discovered(const bridge_discovered_profile_t *profile,
                                                    bridge_profile_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (!profile) {
        return;
    }
    strlcpy(snapshot->id, profile->id, sizeof(snapshot->id));
    strlcpy(snapshot->label, profile->label, sizeof(snapshot->label));
    strlcpy(snapshot->host, profile->host, sizeof(snapshot->host));
    snapshot->port = (int)profile->port;
    strlcpy(snapshot->token, profile->token, sizeof(snapshot->token));
}

static void bridge_profile_snapshot_view(const bridge_profile_snapshot_t *snapshot,
                                         bridge_profile_config_t *view)
{
    *view = (bridge_profile_config_t){
        .id = snapshot->id,
        .label = snapshot->label,
        .host = snapshot->host,
        .port = snapshot->port,
        .token = snapshot->token,
    };
}

static size_t bridge_profile_count(void)
{
    bridge_profiles_lock();
    size_t count = s_discovered_bridge_profile_count;
    bridge_profiles_unlock();
    if (count > 0) {
        return count;
    }
    return sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]);
}

static size_t bridge_saved_profile_count(void)
{
    bridge_profiles_lock();
    size_t count = s_discovered_bridge_profile_count;
    bridge_profiles_unlock();
    return count;
}

static bool bridge_saved_profile_snapshot_at(size_t index,
                                             bridge_profile_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    bridge_profiles_lock();
    if (index >= s_discovered_bridge_profile_count) {
        bridge_profiles_unlock();
        return false;
    }
    bridge_profile_snapshot_from_discovered(&s_discovered_bridge_profiles[index],
                                            snapshot);
    bridge_profiles_unlock();
    return true;
}

static bool bridge_profile_snapshot_at(size_t index, bridge_profile_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    bridge_profiles_lock();
    size_t discovered_count = s_discovered_bridge_profile_count;
    if (discovered_count > 0) {
        if (index >= discovered_count) {
            bridge_profiles_unlock();
            return false;
        }
        bridge_profile_snapshot_from_discovered(&s_discovered_bridge_profiles[index],
                                                snapshot);
        bridge_profiles_unlock();
        return true;
    }
    bridge_profiles_unlock();

    size_t configured_count =
        sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]);
    if (index >= configured_count) {
        return false;
    }
    bridge_profile_snapshot_from_config(&k_configured_bridge_profiles[index], snapshot);
    return true;
}

static bool bridge_target_profile_snapshot(const bridge_target_t *target,
                                           bridge_profile_snapshot_t *snapshot)
{
    if (!target || !snapshot || target->profile_id[0] == '\0') {
        return false;
    }

    bridge_profiles_lock();
    for (size_t index = 0; index < s_discovered_bridge_profile_count; index++) {
        const bridge_discovered_profile_t *profile =
            &s_discovered_bridge_profiles[index];
        if (strcmp(profile->id, target->profile_id) == 0) {
            bridge_profile_snapshot_from_discovered(profile, snapshot);
            bridge_profiles_unlock();
            return true;
        }
    }
    bridge_profiles_unlock();

    size_t configured_count =
        sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]);
    for (size_t index = 0; index < configured_count; index++) {
        const bridge_profile_config_t *profile = &k_configured_bridge_profiles[index];
        if (profile->id && strcmp(profile->id, target->profile_id) == 0) {
            bridge_profile_snapshot_from_config(profile, snapshot);
            return true;
        }
    }
    return false;
}

static void bridge_profile_views_rebuild(void)
{
    for (size_t index = 0; index < s_discovered_bridge_profile_count; index++) {
        bridge_discovered_profile_t *stored = &s_discovered_bridge_profiles[index];
        s_discovered_bridge_profile_views[index] = (bridge_profile_config_t){
            .id = stored->id,
            .label = stored->label,
            .host = stored->host,
            .port = (int)stored->port,
            .token = stored->token,
        };
    }
}

static void bridge_profiles_clear(void)
{
    bridge_profiles_lock();
    memset(s_discovered_bridge_profiles, 0, sizeof(s_discovered_bridge_profiles));
    memset(s_discovered_bridge_profile_views, 0, sizeof(s_discovered_bridge_profile_views));
    s_discovered_bridge_profile_count = 0;
    bridge_profiles_unlock();
}

static esp_err_t bridge_profiles_load_nvs(const char *current_ssid)
{
    bridge_profiles_clear();
    if (!current_ssid || current_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    bridge_profile_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    size_t required_size = sizeof(*store);
    err = nvs_get_blob(handle, BRIDGE_PROFILE_STORE_KEY, store, &required_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(store);
        return err;
    }
    if (required_size != sizeof(*store) ||
        store->magic != BRIDGE_PROFILE_STORE_MAGIC ||
        store->version != BRIDGE_PROFILE_STORE_VERSION ||
        store->count == 0 ||
        store->count > VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT ||
        strcmp(store->ssid, current_ssid) != 0) {
        free(store);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t restored_count = store->count;
    bridge_profiles_lock();
    memcpy(s_discovered_bridge_profiles, store->profiles,
           store->count * sizeof(store->profiles[0]));
    s_discovered_bridge_profile_count = store->count;
    bridge_profile_views_rebuild();
    bridge_profiles_unlock();
    free(store);
    ESP_LOGI(TAG, "bridge profiles restored ssid=%s count=%u",
             current_ssid, (unsigned int)restored_count);
    return ESP_OK;
}

static esp_err_t bridge_profiles_save_nvs(const char *expected_ssid)
{
    char current_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(current_ssid, sizeof(current_ssid));
    if (current_ssid[0] == '\0' ||
        !expected_ssid || strcmp(current_ssid, expected_ssid) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_profile_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        return ESP_ERR_NO_MEM;
    }
    store->magic = BRIDGE_PROFILE_STORE_MAGIC;
    store->version = BRIDGE_PROFILE_STORE_VERSION;
    bridge_profiles_lock();
    if (s_discovered_bridge_profile_count == 0) {
        bridge_profiles_unlock();
        free(store);
        return ESP_ERR_INVALID_STATE;
    }
    store->count = (uint16_t)s_discovered_bridge_profile_count;
    strlcpy(store->ssid, current_ssid, sizeof(store->ssid));
    memcpy(store->profiles, s_discovered_bridge_profiles,
           s_discovered_bridge_profile_count * sizeof(store->profiles[0]));
    bridge_profiles_unlock();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(store);
        ESP_LOGE(TAG, "open bridge profile NVS for write: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, BRIDGE_PROFILE_STORE_KEY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    uint16_t saved_count = store->count;
    nvs_close(handle);
    free(store);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bridge profiles saved ssid=%s count=%u",
                 current_ssid, (unsigned int)saved_count);
    }
    return err;
}

static int bridge_profile_index_by_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return -1;
    }
    size_t count = bridge_profile_count();
    for (size_t index = 0; index < count; index++) {
        bridge_profile_snapshot_t profile;
        if (bridge_profile_snapshot_at(index, &profile) &&
            strcmp(profile.id, id) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool bridge_target_set_profile(size_t profile_index, const char *source, bool available)
{
    bridge_profile_snapshot_t profile;
    if (!bridge_profile_snapshot_at(profile_index, &profile) ||
        profile.id[0] == '\0' || profile.host[0] == '\0' || profile.port <= 0) {
        return false;
    }
    char ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(ssid, sizeof(ssid));
    if (s_bridge_target_lock) {
        xSemaphoreTake(s_bridge_target_lock, portMAX_DELAY);
    }
    strlcpy(s_bridge_target.host, profile.host, sizeof(s_bridge_target.host));
    s_bridge_target.port = profile.port;
    s_bridge_target.profile_index = profile_index;
    strlcpy(s_bridge_target.profile_id, profile.id, sizeof(s_bridge_target.profile_id));
    strlcpy(s_bridge_target.source, source ? source : "runtime",
            sizeof(s_bridge_target.source));
    strlcpy(s_bridge_target.ssid, ssid, sizeof(s_bridge_target.ssid));
    s_bridge_target.failure_count = 0;
    s_bridge_target.available = available;
    if (s_bridge_target_lock) {
        xSemaphoreGive(s_bridge_target_lock);
    }
    return true;
}

static void bridge_target_note_result(const char *expected_profile_id,
                                      esp_err_t err)
{
    if (s_bridge_target_lock) {
        xSemaphoreTake(s_bridge_target_lock, portMAX_DELAY);
    }
    if (expected_profile_id && expected_profile_id[0] != '\0' &&
        strcmp(s_bridge_target.profile_id, expected_profile_id) != 0) {
        if (s_bridge_target_lock) {
            xSemaphoreGive(s_bridge_target_lock);
        }
        ESP_LOGI(TAG, "bridge result ignored for stale profile id=%s",
                 expected_profile_id);
        return;
    }
    if (err == ESP_OK) {
        s_bridge_target.failure_count = 0;
        s_bridge_target.available = true;
    } else {
        s_bridge_target.failure_count++;
        s_bridge_target.available = false;
    }
    if (s_bridge_target_lock) {
        xSemaphoreGive(s_bridge_target_lock);
    }
}

static bool bridge_target_needs_selection(void)
{
    bridge_target_t target;
    bridge_profile_snapshot_t profile;
    bridge_target_copy(&target);
    return target.host[0] == '\0' || target.port <= 0 ||
           !bridge_target_profile_snapshot(&target, &profile);
}

static esp_err_t bridge_target_load_nvs(void)
{
    char current_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(current_ssid, sizeof(current_ssid));
    if (current_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open bridge target NVS");

    char ssid[WIFI_PROFILE_SSID_LEN] = {0};
    char profile_id[BRIDGE_TARGET_PROFILE_LEN] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t profile_id_len = sizeof(profile_id);
    err = nvs_get_str(handle, BRIDGE_TARGET_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, BRIDGE_TARGET_PROFILE_KEY, profile_id, &profile_id_len);
    }
    nvs_close(handle);
    if (err != ESP_OK || strcmp(ssid, current_ssid) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int profile_index = bridge_profile_index_by_id(profile_id);
    if (profile_index < 0 ||
        !bridge_target_set_profile((size_t)profile_index, "nvs", false)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "bridge profile restored ssid=%s id=%s",
             current_ssid, profile_id);
    return ESP_OK;
}

static esp_err_t bridge_target_save_nvs(void)
{
    bridge_target_t target;
    bridge_target_copy(&target);
    if (target.profile_id[0] == '\0' || target.ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open bridge target NVS for write");
    err = nvs_set_str(handle, BRIDGE_TARGET_SSID_KEY, target.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, BRIDGE_TARGET_PROFILE_KEY, target.profile_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, BRIDGE_TARGET_HOST_KEY, target.host);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, BRIDGE_TARGET_PORT_KEY, target.port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bridge profile saved ssid=%s id=%s source=%s",
                 target.ssid, target.profile_id, target.source);
    }
    return err;
}

static void set_common_http_headers(esp_http_client_handle_t client, const char *token)
{
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name", FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version", FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport", TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date", FIRMWARE_BUILD_ID);
    esp_http_client_set_header(client, "X-Vibe-Stick-Board", VIBE_BOARD_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Device-Ip", s_state.wifi_ip);

    char id[18] = {0};
    char ssid[WIFI_PROFILE_SSID_LEN] = {0};
    char bssid[18] = {0};
    char rssi[8] = {0};
    char wake_code[12] = {0};
    char wake_ext1[20] = {0};
    char reset_code[12] = {0};
    char boot_count[12] = {0};
    char pmic_wake[8] = {0};
    char pmic_irq[24] = {0};
    char pmic_timer[24] = {0};
    char pmic_gpio_wake[16] = {0};
    device_id(id, sizeof(id));
    current_wifi_ssid(ssid, sizeof(ssid));
    current_wifi_bssid(bssid, sizeof(bssid));
    snprintf(rssi, sizeof(rssi), "%d", current_wifi_rssi());
    snprintf(wake_code, sizeof(wake_code), "%d", (int)s_boot_wake_cause);
    snprintf(wake_ext1, sizeof(wake_ext1), "0x%llx",
             (unsigned long long)s_boot_ext1_wake_status);
    snprintf(reset_code, sizeof(reset_code), "%d", (int)s_boot_reset_reason);
    snprintf(boot_count, sizeof(boot_count), "%lu",
             (unsigned long)s_retained_boot_count);
    snprintf(pmic_wake, sizeof(pmic_wake), "0x%02x",
             s_boot_power_status.wake_source);
    snprintf(pmic_irq, sizeof(pmic_irq), "%02x/%02x/%02x",
             s_boot_power_status.irq_status_gpio,
             s_boot_power_status.irq_status_power,
             s_boot_power_status.irq_status_button);
    snprintf(pmic_timer, sizeof(pmic_timer), "%02x/%lu",
             s_boot_power_status.timer_config,
             (unsigned long)s_boot_power_status.timer_seconds);
    snprintf(pmic_gpio_wake, sizeof(pmic_gpio_wake), "%02x/%02x",
             s_boot_power_status.gpio_wake_enable,
             s_boot_power_status.gpio_wake_config);
    esp_http_client_set_header(client, "X-Vibe-Stick-Device-Id", id);
    esp_http_client_set_header(client, "X-Vibe-Stick-Wifi-Ssid", ssid);
    esp_http_client_set_header(client, "X-Vibe-Stick-Wifi-Bssid", bssid);
    esp_http_client_set_header(client, "X-Vibe-Stick-Wifi-Rssi", rssi);
    esp_http_client_set_header(client, "X-Vibe-Stick-Wake-Cause",
                               wake_cause_label(s_boot_wake_cause));
    esp_http_client_set_header(client, "X-Vibe-Stick-Wake-Cause-Code", wake_code);
    esp_http_client_set_header(client, "X-Vibe-Stick-Wake-Ext1", wake_ext1);
    esp_http_client_set_header(client, "X-Vibe-Stick-Reset-Reason",
                               reset_reason_label(s_boot_reset_reason));
    esp_http_client_set_header(client, "X-Vibe-Stick-Reset-Reason-Code", reset_code);
    esp_http_client_set_header(client, "X-Vibe-Stick-Boot-Count", boot_count);
    if (s_boot_power_status.available) {
        esp_http_client_set_header(client, "X-Vibe-Stick-Pmic-Wake", pmic_wake);
        esp_http_client_set_header(client, "X-Vibe-Stick-Pmic-Irq", pmic_irq);
        esp_http_client_set_header(client, "X-Vibe-Stick-Pmic-Timer", pmic_timer);
        esp_http_client_set_header(client, "X-Vibe-Stick-Pmic-Gpio-Wake",
                                   pmic_gpio_wake);
    }
    if (token && token[0] != '\0') {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", token);
    }
}

static esp_err_t http_request_target(const char *method, const char *host, int port,
                                     const char *token, const char *path, const char *body,
                                     char *response, int response_len, int timeout_ms)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
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
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_BUFFER_SIZE,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    set_common_http_headers(client, token);
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

static bool bridge_health_name_valid(const cJSON *bridge_name)
{
    return cJSON_IsString(bridge_name) &&
           (strcmp(bridge_name->valuestring, "vibestick-bridge") == 0 ||
            strcmp(bridge_name->valuestring, "capswriter-m5-voice-bridge") == 0);
}

static void bridge_discovery_fallback_id(const char *host, char *id, size_t id_len)
{
    if (!id || id_len == 0) {
        return;
    }
    strlcpy(id, "lan-", id_len);
    strlcat(id, host && host[0] != '\0' ? host : "bridge", id_len);
    for (char *cursor = id; *cursor != '\0'; cursor++) {
        if (*cursor == '.') {
            *cursor = '-';
        }
    }
}

static bool bridge_parse_discovered_health(const char *response, const char *host, int port,
                                           const char *token,
                                           bridge_discovered_profile_t *profile)
{
    if (!response || !host || !profile) {
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return false;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *bridge_name = cJSON_GetObjectItemCaseSensitive(root, "bridge_name");
    cJSON *bridge_id = cJSON_GetObjectItemCaseSensitive(root, "bridge_id");
    cJSON *bridge_label = cJSON_GetObjectItemCaseSensitive(root, "bridge_label");
    bool healthy = cJSON_IsBool(ok) && cJSON_IsTrue(ok) &&
                   bridge_health_name_valid(bridge_name);
    if (!healthy) {
        cJSON_Delete(root);
        return false;
    }

    memset(profile, 0, sizeof(*profile));
    strlcpy(profile->host, host, sizeof(profile->host));
    profile->port = port;
    strlcpy(profile->token, token ? token : "", sizeof(profile->token));

    bool generic_id = !cJSON_IsString(bridge_id) ||
                      bridge_id->valuestring[0] == '\0' ||
                      strcmp(bridge_id->valuestring, "capswriter-m5-voice-bridge") == 0 ||
                      strcmp(bridge_id->valuestring, "vibestick-bridge") == 0;
    if (generic_id) {
        bridge_discovery_fallback_id(host, profile->id, sizeof(profile->id));
    } else {
        strlcpy(profile->id, bridge_id->valuestring, sizeof(profile->id));
    }

    bool generic_label = !cJSON_IsString(bridge_label) ||
                         bridge_label->valuestring[0] == '\0' ||
                         strcmp(bridge_label->valuestring,
                                "capswriter-m5-voice-bridge") == 0 ||
                         strcmp(bridge_label->valuestring, "vibestick-bridge") == 0;
    strlcpy(profile->label,
            generic_label ? host : bridge_label->valuestring,
            sizeof(profile->label));
    cJSON_Delete(root);
    return true;
}

static bool bridge_probe_discovered(const char *host, int port,
                                    bridge_discovered_profile_t *profile)
{
    char response[BRIDGE_HEALTH_RESPONSE_BYTES] = {0};
    esp_err_t anonymous_err =
        http_request_target("GET", host, port, "", "/health", NULL, response,
                            sizeof(response), BRIDGE_DISCOVERY_HEALTH_TIMEOUT_MS);
    if (anonymous_err == ESP_OK &&
        bridge_parse_discovered_health(response, host, port, "", profile)) {
        return true;
    }

    size_t configured_count =
        sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]);
    for (size_t index = 0; index < configured_count; index++) {
        const char *token = k_configured_bridge_profiles[index].token;
        if (!token || token[0] == '\0') {
            continue;
        }
        bool already_tried = false;
        for (size_t previous = 0; previous < index; previous++) {
            const char *previous_token = k_configured_bridge_profiles[previous].token;
            if (previous_token && strcmp(previous_token, token) == 0) {
                already_tried = true;
                break;
            }
        }
        if (already_tried) {
            continue;
        }
        response[0] = '\0';
        if (http_request_target("GET", host, port, token, "/health", NULL, response,
                                sizeof(response), BRIDGE_DISCOVERY_HEALTH_TIMEOUT_MS) == ESP_OK &&
            bridge_parse_discovered_health(response, host, port, token, profile)) {
            return true;
        }
    }
    return false;
}

static bool bridge_probe_profile(const bridge_profile_config_t *profile, int timeout_ms)
{
    char response[BRIDGE_HEALTH_RESPONSE_BYTES] = {0};
    if (!profile || !profile->host || !profile->id || profile->host[0] == '\0' ||
        profile->id[0] == '\0') {
        return false;
    }
    esp_err_t err = http_request_target("GET", profile->host, profile->port, profile->token,
                                        "/health", NULL, response, sizeof(response), timeout_ms);
    if (err != ESP_OK || response[0] == '\0') {
        return false;
    }
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return false;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *bridge_name = cJSON_GetObjectItemCaseSensitive(root, "bridge_name");
    cJSON *bridge_id = cJSON_GetObjectItemCaseSensitive(root, "bridge_id");
    bool generic_profile_id = strncmp(profile->id, "lan-", 4) == 0;
    bool healthy = cJSON_IsBool(ok) && cJSON_IsTrue(ok) &&
                   bridge_health_name_valid(bridge_name) &&
                   (generic_profile_id ||
                    (cJSON_IsString(bridge_id) &&
                     strcmp(bridge_id->valuestring, profile->id) == 0));
    cJSON_Delete(root);
    return healthy;
}

static bool bridge_scan_add(const bridge_discovered_profile_t *profile)
{
    if (!profile || profile->host[0] == '\0' ||
        s_bridge_scan_profile_count >= VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT) {
        return false;
    }
    for (size_t index = 0; index < s_bridge_scan_profile_count; index++) {
        if (strcmp(s_bridge_scan_profiles[index].host, profile->host) == 0 &&
            s_bridge_scan_profiles[index].port == profile->port) {
            return false;
        }
    }
    s_bridge_scan_profiles[s_bridge_scan_profile_count++] = *profile;
    return true;
}

static void bridge_wait_for_socket_connections(int *sockets, bool *connected,
                                               size_t socket_count, int timeout_ms)
{
    bool settled[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT] = {0};
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (size_t index = 0; index < socket_count; index++) {
        settled[index] = connected[index];
    }

    while (true) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        int max_socket = -1;
        size_t pending = 0;
        for (size_t index = 0; index < socket_count; index++) {
            if (settled[index]) {
                continue;
            }
            FD_SET(sockets[index], &write_fds);
            if (sockets[index] > max_socket) {
                max_socket = sockets[index];
            }
            pending++;
        }
        if (pending == 0) {
            return;
        }

        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return;
        }
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining_us / 1000000),
            .tv_usec = (suseconds_t)(remaining_us % 1000000),
        };
        int ready = select(max_socket + 1, NULL, &write_fds, NULL, &timeout);
        if (ready <= 0) {
            return;
        }
        for (size_t index = 0; index < socket_count; index++) {
            if (settled[index] || !FD_ISSET(sockets[index], &write_fds)) {
                continue;
            }
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            connected[index] =
                getsockopt(sockets[index], SOL_SOCKET, SO_ERROR,
                           &socket_error, &error_len) == 0 &&
                socket_error == 0;
            settled[index] = true;
        }
    }
}

static size_t bridge_discover_subnet_profiles(void)
{
    char scan_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(scan_ssid, sizeof(scan_ssid));
    if (scan_ssid[0] == '\0') {
        return 0;
    }
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int self = 0;
    if (sscanf(s_state.wifi_ip, "%u.%u.%u.%u", &a, &b, &c, &self) != 4 ||
        a > 255 || b > 255 || c > 255 || self > 255) {
        return 0;
    }

    memset(s_bridge_scan_profiles, 0, sizeof(s_bridge_scan_profiles));
    s_bridge_scan_profile_count = 0;
    ESP_LOGI(TAG, "bridge discovery start prefix=%u.%u.%u.0/24 port=%d",
             a, b, c, VIBE_STICK_BRIDGE_PORT);
    int next_host_id = 254;
    while (next_host_id >= 1 &&
           s_bridge_scan_profile_count < VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT) {
        bool logged_pause = false;
        while (recording_network_busy()) {
            if (!logged_pause) {
                ESP_LOGI(TAG, "bridge discovery paused while recording network is busy");
                logged_pause = true;
            }
            vTaskDelay(pdMS_TO_TICKS(BRIDGE_DISCOVERY_PAUSE_POLL_MS));
        }
        bridge_probe_lock();
        int sockets[BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE];
        int host_ids[BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE];
        bool connected[BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE] = {0};
        size_t socket_count = 0;

        while (next_host_id >= 1 &&
               socket_count < BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE) {
            int host_id = next_host_id--;
            if ((unsigned int)host_id == self) {
                continue;
            }
            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                continue;
            }
            int flags = fcntl(sock, F_GETFL, 0);
            if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
                close(sock);
                continue;
            }
            struct sockaddr_in address = {
                .sin_family = AF_INET,
                .sin_port = htons((uint16_t)VIBE_STICK_BRIDGE_PORT),
            };
            char host[BRIDGE_TARGET_HOST_LEN] = {0};
            snprintf(host, sizeof(host), "%u.%u.%u.%d", a, b, c, host_id);
            if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
                close(sock);
                continue;
            }
            int result = connect(sock, (struct sockaddr *)&address, sizeof(address));
            if (result < 0 && errno != EINPROGRESS) {
                close(sock);
                continue;
            }
            sockets[socket_count] = sock;
            host_ids[socket_count] = host_id;
            connected[socket_count] = result == 0;
            socket_count++;
        }

        bridge_wait_for_socket_connections(
            sockets, connected, socket_count, BRIDGE_DISCOVERY_CONNECT_TIMEOUT_MS);
        for (size_t index = 0; index < socket_count; index++) {
            bool port_open = connected[index];
            close(sockets[index]);
            if (!port_open) {
                continue;
            }
            char host[BRIDGE_TARGET_HOST_LEN] = {0};
            snprintf(host, sizeof(host), "%u.%u.%u.%d",
                     a, b, c, host_ids[index]);
            while (recording_network_busy()) {
                if (!logged_pause) {
                    ESP_LOGI(TAG, "bridge discovery paused while recording network is busy");
                    logged_pause = true;
                }
                vTaskDelay(pdMS_TO_TICKS(BRIDGE_DISCOVERY_PAUSE_POLL_MS));
            }
            bridge_discovered_profile_t profile = {0};
            if (!bridge_probe_discovered(host, VIBE_STICK_BRIDGE_PORT, &profile)) {
                continue;
            }
            if (bridge_scan_add(&profile)) {
                ESP_LOGI(TAG, "bridge discovered id=%s label=%s host=%s port=%ld",
                         profile.id, profile.label, profile.host, (long)profile.port);
            }
        }
        bridge_probe_unlock();
    }
    if (s_bridge_scan_profile_count == 0) {
        ESP_LOGW(TAG, "bridge discovery found no bridge prefix=%u.%u.%u.0/24",
                 a, b, c);
        return 0;
    }

    ESP_LOGI(TAG, "bridge discovery complete count=%u",
             (unsigned int)s_bridge_scan_profile_count);
    return s_bridge_scan_profile_count;
}

static bool bridge_discovered_profile_equal(const bridge_discovered_profile_t *a,
                                            const bridge_discovered_profile_t *b)
{
    return a && b &&
           strcmp(a->id, b->id) == 0 &&
           strcmp(a->label, b->label) == 0 &&
           strcmp(a->host, b->host) == 0 &&
           a->port == b->port &&
           strcmp(a->token, b->token) == 0;
}

static int bridge_discovered_profile_find_locked(const bridge_discovered_profile_t *profile)
{
    if (!profile || profile->id[0] == '\0') {
        return -1;
    }
    for (size_t index = 0; index < s_discovered_bridge_profile_count; index++) {
        bridge_discovered_profile_t *stored = &s_discovered_bridge_profiles[index];
        if ((stored->id[0] != '\0' && strcmp(stored->id, profile->id) == 0) ||
            (strcmp(stored->host, profile->host) == 0 &&
             stored->port == profile->port)) {
            return (int)index;
        }
    }
    return -1;
}

static bool bridge_profiles_merge_scan_results(const char *scan_ssid)
{
    char current_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(current_ssid, sizeof(current_ssid));
    if (!scan_ssid || scan_ssid[0] == '\0' ||
        strcmp(current_ssid, scan_ssid) != 0) {
        return false;
    }

    bool changed = false;
    bridge_profiles_lock();
    for (size_t scan_index = 0;
         scan_index < s_bridge_scan_profile_count;
         scan_index++) {
        const bridge_discovered_profile_t *profile =
            &s_bridge_scan_profiles[scan_index];
        int existing = bridge_discovered_profile_find_locked(profile);
        if (existing >= 0) {
            bridge_discovered_profile_t *stored =
                &s_discovered_bridge_profiles[(size_t)existing];
            if (!bridge_discovered_profile_equal(stored, profile)) {
                *stored = *profile;
                changed = true;
            }
        } else {
            if (s_discovered_bridge_profile_count >=
                VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT) {
                ESP_LOGW(TAG, "bridge profile store full; skipping id=%s host=%s",
                         profile->id, profile->host);
            } else {
                s_discovered_bridge_profiles[s_discovered_bridge_profile_count++] =
                    *profile;
                changed = true;
            }
        }
    }
    if (changed) {
        bridge_profile_views_rebuild();
    }
    bridge_profiles_unlock();

    if (!changed) {
        return false;
    }
    esp_err_t err = bridge_profiles_save_nvs(scan_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bridge profile NVS save skipped/failed: %s",
                 esp_err_to_name(err));
    }
    return true;
}

static void bridge_discovery_task(void *arg)
{
    (void)arg;
    char scan_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(scan_ssid, sizeof(scan_ssid));
    size_t count = bridge_discover_subnet_profiles();
    if (count > 0) {
        (void)bridge_profiles_merge_scan_results(scan_ssid);
        render_state();
        if (!atomic_load(&s_bridge_selection_active)) {
            char summary[24];
            snprintf(summary, sizeof(summary), "%u BRIDGES",
                     (unsigned int)count);
            show_mode_switch_visual("SCAN COMPLETE", summary,
                                    s_pet_done_frames,
                                    sizeof(s_pet_done_frames) /
                                        sizeof(s_pet_done_frames[0]),
                                    lv_color_hex(0x86efac));
        }
    } else if (!atomic_load(&s_bridge_selection_active)) {
        show_mode_switch_visual("NO BRIDGE", "OFFLINE",
                                s_mode_switch_dict_frames,
                                sizeof(s_mode_switch_dict_frames) /
                                    sizeof(s_mode_switch_dict_frames[0]),
                                lv_color_hex(0xfca5a5));
    }
    atomic_store(&s_bridge_discovery_active, false);
    s_bridge_discovery_task = NULL;
    vTaskDelete(NULL);
}

static bool start_bridge_discovery_task(bool show_searching)
{
    if (!wifi_connected()) {
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_bridge_discovery_active,
                                        &expected, true)) {
        ESP_LOGI(TAG, "bridge discovery already running");
        if (show_searching && !atomic_load(&s_bridge_selection_active)) {
            show_persistent_mode_switch_visual(
                "SEARCHING", "LAN BRIDGES",
                s_mode_switch_dict_frames,
                sizeof(s_mode_switch_dict_frames) /
                    sizeof(s_mode_switch_dict_frames[0]),
                lv_color_hex(0x93c5fd));
        }
        return false;
    }
    if (show_searching && !atomic_load(&s_bridge_selection_active)) {
        show_persistent_mode_switch_visual("SEARCHING",
                                           "LAN BRIDGES",
                                           s_mode_switch_dict_frames,
                                           sizeof(s_mode_switch_dict_frames) /
                                               sizeof(s_mode_switch_dict_frames[0]),
                                           lv_color_hex(0x93c5fd));
    }
    BaseType_t ok =
        xTaskCreatePinnedToCore(bridge_discovery_task, "bridge_scan", 8192,
                                NULL, 3, &s_bridge_discovery_task,
                                VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        atomic_store(&s_bridge_discovery_active, false);
        s_bridge_discovery_task = NULL;
        ESP_LOGW(TAG, "bridge discovery task create failed");
        return false;
    }
    return true;
}

static void bridge_ensure_target(void)
{
    if (!wifi_connected()) {
        return;
    }
    char current_ssid[WIFI_PROFILE_SSID_LEN] = {0};
    current_wifi_ssid(current_ssid, sizeof(current_ssid));
    if (current_ssid[0] != '\0' &&
        strcmp(s_bridge_profiles_loaded_ssid, current_ssid) != 0) {
        (void)bridge_profiles_load_nvs(current_ssid);
        (void)bridge_target_load_nvs();
        strlcpy(s_bridge_profiles_loaded_ssid, current_ssid,
                sizeof(s_bridge_profiles_loaded_ssid));
    }
    if (!bridge_target_needs_selection()) {
        return;
    }
    if (bridge_target_set_profile(0, "default", false)) {
        (void)bridge_target_save_nvs();
    }
}

static void cycle_bridge_profile(void)
{
    if (recording_network_busy()) {
        ESP_LOGI(TAG, "bridge profile switch ignored while recording");
        return;
    }
    bridge_ensure_target();
    bridge_target_t current;
    bridge_target_copy(&current);
    size_t count = bridge_saved_profile_count();
    if (count == 0) {
        ESP_LOGW(TAG, "bridge selection has no saved profiles");
        show_bridge_selection_visual("OFFLINE", lv_color_hex(0xfca5a5));
        return;
    }

    int current_index = -1;
    for (size_t index = 0; index < count; index++) {
        bridge_profile_snapshot_t profile;
        if (!bridge_saved_profile_snapshot_at(index, &profile)) {
            continue;
        }
        if (strcmp(profile.id, current.profile_id) == 0 ||
            strcmp(profile.host, current.host) == 0) {
            current_index = (int)index;
            break;
        }
    }

    size_t next_index =
        current_index >= 0 ? ((size_t)current_index + 1) % count : 0;
    bridge_profile_snapshot_t next;
    if (!bridge_saved_profile_snapshot_at(next_index, &next) ||
        !bridge_target_set_profile(next_index, "manual", false)) {
        ESP_LOGW(TAG, "bridge selection failed index=%u",
                 (unsigned int)next_index);
        show_bridge_selection_visual("OFFLINE", lv_color_hex(0xfca5a5));
        return;
    }
    (void)bridge_target_save_nvs();
    ESP_LOGI(TAG, "bridge profile selected id=%s host=%s port=%d",
             next.id, next.host, next.port);
    render_state();
    show_bridge_selection_visual("CONNECTING", lv_color_hex(0x93c5fd));
    (void)queue_event(VIBE_STICK_EVENT_POLL_STATE);
}

static esp_err_t bridge_prepare_active_target(bridge_target_t *target)
{
    bridge_ensure_target();
    bridge_target_t current;
    bridge_target_copy(&current);
    bridge_profile_snapshot_t profile;
    bridge_profile_config_t profile_view;
    if (!bridge_target_profile_snapshot(&current, &profile)) {
        return ESP_ERR_NOT_FOUND;
    }
    bridge_profile_snapshot_view(&profile, &profile_view);
    if (!current.available) {
        if (!bridge_probe_profile(&profile_view, 1200)) {
            bridge_target_note_result(current.profile_id, ESP_FAIL);
            return ESP_ERR_NOT_FOUND;
        }
        int refreshed_index = bridge_profile_index_by_id(profile.id);
        if (refreshed_index >= 0 &&
            (strcmp(current.host, profile.host) != 0 ||
             current.port != profile.port)) {
            if (!bridge_target_set_profile((size_t)refreshed_index,
                                           "rediscovered", true)) {
                return ESP_ERR_NOT_FOUND;
            }
            (void)bridge_target_save_nvs();
            ESP_LOGI(TAG, "bridge target refreshed id=%s host=%s port=%d",
                     profile.id, profile.host, profile.port);
        } else {
            bridge_target_note_result(current.profile_id, ESP_OK);
        }
        bridge_target_copy(&current);
    }
    if (target) {
        *target = current;
    }
    return ESP_OK;
}

static esp_err_t http_request_timeout(const char *method, const char *path, const char *body,
                                      char *response, int response_len, int timeout_ms)
{
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG, "prepare bridge target");
    bridge_profile_snapshot_t profile;
    const char *token = bridge_target_profile_snapshot(&target, &profile)
                            ? profile.token
                            : VIBE_STICK_BRIDGE_TOKEN;
    esp_err_t err = http_request_target(method, target.host, target.port, token, path, body,
                                        response, response_len, timeout_ms);
    bridge_target_note_result(target.profile_id, err);
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
    char url[256];
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG, "prepare bridge target");
    snprintf(url, sizeof(url), "http://%s:%d%s", target.host, target.port, path);
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
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_BUFFER_SIZE,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    bridge_profile_snapshot_t profile;
    set_common_http_headers(client,
                            bridge_target_profile_snapshot(&target, &profile)
                                ? profile.token
                                : VIBE_STICK_BRIDGE_TOKEN);
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
    bridge_target_note_result(target.profile_id, err);
    return err;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool wav_pcm16_mono_payload(const uint8_t *data, size_t len,
                                   const uint8_t **pcm, size_t *pcm_len)
{
    if (!data || len < 44 || !pcm || !pcm_len) {
        return false;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    bool format_ok = false;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    while (offset + 8 <= len) {
        const uint8_t *chunk = data + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t data_offset = offset + 8;
        if (data_offset + chunk_size > len) {
            return false;
        }
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint16_t audio_format = read_le16(data + data_offset);
            uint16_t channels = read_le16(data + data_offset + 2);
            uint32_t sample_rate = read_le32(data + data_offset + 4);
            uint16_t bits_per_sample = read_le16(data + data_offset + 14);
            format_ok = audio_format == 1 &&
                        channels == VIBE_STICK_AUDIO_CHANNELS &&
                        sample_rate == VIBE_STICK_AUDIO_SAMPLE_RATE &&
                        bits_per_sample == VIBE_STICK_AUDIO_BITS_PER_SAMPLE;
        } else if (memcmp(chunk, "data", 4) == 0) {
            payload = data + data_offset;
            payload_len = chunk_size;
        }
        offset = data_offset + chunk_size + (chunk_size & 1u);
    }
    if (!format_ok || !payload || payload_len == 0 || (payload_len % 2) != 0) {
        return false;
    }
    *pcm = payload;
    *pcm_len = payload_len;
    return true;
}

static esp_err_t download_tts_audio(uint8_t **audio, size_t *audio_len)
{
    if (!audio || !audio_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *audio = NULL;
    *audio_len = 0;

    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG, "prepare bridge target");
    char url[256];
    build_bridge_url(VIBE_STICK_RECORDING_TTS_PATH, url, sizeof(url));
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_BUFFER_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "tts http init");
    bridge_profile_snapshot_t profile;
    set_common_http_headers(client,
                            bridge_target_profile_snapshot(&target, &profile)
                                ? profile.token
                                : VIBE_STICK_BRIDGE_TOKEN);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200 || content_length <= 0 || content_length > TTS_AUDIO_MAX_BYTES) {
        ESP_LOGW(TAG, "tts download rejected status=%d length=%lld",
                 status_code, (long long)content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *buffer = heap_caps_malloc((size_t)content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = heap_caps_malloc((size_t)content_length, MALLOC_CAP_8BIT);
    }
    if (!buffer) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < (size_t)content_length) {
        int read = esp_http_client_read(client, (char *)buffer + total,
                                        (int)((size_t)content_length - total));
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        total += (size_t)read;
    }

    if (err == ESP_OK && total != (size_t)content_length) {
        err = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    bridge_target_note_result(target.profile_id, err);
    if (err != ESP_OK) {
        heap_caps_free(buffer);
        return err;
    }
    *audio = buffer;
    *audio_len = total;
    return ESP_OK;
}

static esp_err_t play_latest_tts_audio(void)
{
    uint8_t *audio = NULL;
    size_t audio_len = 0;
    esp_err_t err = download_tts_audio(&audio, &audio_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tts audio download failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    if (!wav_pcm16_mono_payload(audio, audio_len, &pcm, &pcm_len)) {
        ESP_LOGW(TAG, "tts audio is not 16k mono pcm wav bytes=%u", (unsigned)audio_len);
        heap_caps_free(audio);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "playing tts audio pcm_bytes=%u", (unsigned)pcm_len);
    err = vibe_audio_play_pcm16_mono(pcm, pcm_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tts audio playback failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(audio);
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
    copy_json_string(root, "sha256", manifest->sha256, sizeof(manifest->sha256));
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
    bridge_ensure_target();
    bridge_target_t target;
    bridge_target_copy(&target);
    snprintf(url, url_len, "http://%s:%d%s", target.host, target.port, path_or_url);
}

static bool ota_parse_semantic_version(const char *raw, uint32_t version[3])
{
    if (!raw || !version) {
        return false;
    }

    const char *cursor = raw;
    if (*cursor == 'v' || *cursor == 'V') {
        cursor++;
    }
    for (size_t index = 0; index < 3; index++) {
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || value > UINT32_MAX) {
            return false;
        }
        version[index] = (uint32_t)value;
        if (index < 2) {
            if (*end != '.') {
                return false;
            }
            cursor = end + 1;
        } else if (*end != '\0' && *end != '-' && *end != '+') {
            return false;
        }
    }
    return true;
}

static bool ota_compare_semantic_versions(const char *candidate,
                                          const char *current,
                                          int *comparison)
{
    uint32_t candidate_version[3] = {0};
    uint32_t current_version[3] = {0};
    if (!comparison ||
        !ota_parse_semantic_version(candidate, candidate_version) ||
        !ota_parse_semantic_version(current, current_version)) {
        return false;
    }
    for (size_t index = 0; index < 3; index++) {
        if (candidate_version[index] < current_version[index]) {
            *comparison = -1;
            return true;
        }
        if (candidate_version[index] > current_version[index]) {
            *comparison = 1;
            return true;
        }
    }
    *comparison = 0;
    return true;
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
    int version_comparison = 0;
    if (!ota_compare_semantic_versions(manifest->version, FIRMWARE_VERSION,
                                       &version_comparison)) {
        ESP_LOGW(TAG, "OTA manifest version is invalid candidate=%s current=%s",
                 manifest->version, FIRMWARE_VERSION);
        return false;
    }
    if (version_comparison < 0) {
        ESP_LOGW(TAG, "OTA manifest version is older candidate=%s current=%s",
                 manifest->version, FIRMWARE_VERSION);
        return false;
    }
    if (version_comparison == 0) {
        ESP_LOGI(TAG, "OTA manifest version is not newer candidate=%s current=%s",
                 manifest->version, FIRMWARE_VERSION);
        return false;
    }
    if (manifest->sha256[0] != '\0') {
        uint8_t running_sha256[32] = {0};
        const esp_partition_t *running_partition = esp_ota_get_running_partition();
        if (running_partition &&
            esp_partition_get_sha256(running_partition, running_sha256) == ESP_OK) {
            char current_sha256[65] = {0};
            for (size_t i = 0; i < sizeof(running_sha256); i++) {
                snprintf(current_sha256 + i * 2, sizeof(current_sha256) - i * 2,
                         "%02x", running_sha256[i]);
            }
            if (strcmp(manifest->sha256, current_sha256) == 0) {
                ESP_LOGI(TAG, "OTA manifest is current image sha256=%.12s",
                         manifest->sha256);
                return false;
            }
            ESP_LOGI(TAG, "OTA manifest is new image sha256=%.12s current=%.12s",
                     manifest->sha256, current_sha256);
            return true;
        }
        ESP_LOGW(TAG, "OTA running partition sha256 unavailable; falling back");
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

    char url[256];
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG, "prepare bridge target");
    build_bridge_url(path_or_url, url, sizeof(url));
    ESP_LOGI(TAG, "OTA update available board=%s version=%s build=%s size=%d partition=%s",
             manifest->board, manifest->version, manifest->build_id, manifest->size,
             update_partition->label);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_BUFFER_SIZE,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "OTA http init");
    bridge_profile_snapshot_t profile;
    set_common_http_headers(client,
                            bridge_target_profile_snapshot(&target, &profile)
                                ? profile.token
                                : VIBE_STICK_BRIDGE_TOKEN);

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
    const int64_t download_started_ms = esp_timer_get_time() / 1000;
    int64_t last_progress_ms = download_started_ms;
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
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - download_started_ms >= OTA_DOWNLOAD_TIMEOUT_MS ||
                now_ms - last_progress_ms >= OTA_NO_PROGRESS_TIMEOUT_MS) {
                ESP_LOGW(TAG, "OTA download timed out bytes=%d elapsed=%lld idle=%lld",
                         total_read,
                         (long long)(now_ms - download_started_ms),
                         (long long)(now_ms - last_progress_ms));
                err = ESP_ERR_TIMEOUT;
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
        last_progress_ms = esp_timer_get_time() / 1000;
        if (last_progress_ms - download_started_ms >= OTA_DOWNLOAD_TIMEOUT_MS) {
            ESP_LOGW(TAG, "OTA download exceeded total timeout bytes=%d elapsed=%lld",
                     total_read,
                     (long long)(last_progress_ms - download_started_ms));
            err = ESP_ERR_TIMEOUT;
            break;
        }
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
    bool overlay_shown = false;

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
        overlay_shown = true;
        err = perform_ota_update(&manifest);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA update failed: %s", esp_err_to_name(err));
            show_recording_overlay("OTA failed", "", true);
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
    }

    if (overlay_shown) {
        show_recording_overlay(NULL, NULL, false);
    }
    set_ota_in_progress(false);
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

static void start_ota_check_task(void)
{
#if !VIBE_STICK_OTA_ENABLED
    ESP_LOGI(TAG, "OTA check skipped: preview firmware");
    return;
#endif
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
    char tts_playback_request_id[sizeof(s_last_tts_playback_request_id)] = {0};
    copy_json_string(state_root, "tts_playback_request_id",
                     tts_playback_request_id, sizeof(tts_playback_request_id));
    if (tts_playback_request_id[0] != '\0' &&
        strcmp(tts_playback_request_id, s_last_tts_playback_request_id) != 0) {
        strlcpy(s_last_tts_playback_request_id, tts_playback_request_id,
                sizeof(s_last_tts_playback_request_id));
        if (!recording_network_busy()) {
            (void)queue_event(VIBE_STICK_EVENT_TTS_PROBE);
        } else {
            ESP_LOGI(TAG, "tts probe deferred while recording network is busy");
        }
    }
    cJSON_Delete(root);
    return true;
}

static int clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

static void record_battery_sample(int raw_level)
{
    s_battery_samples[s_battery_sample_index] = clamp_percent(raw_level);
    s_battery_sample_index = (s_battery_sample_index + 1) % VIBE_STICK_BATTERY_SAMPLE_COUNT;
    if (s_battery_sample_count < VIBE_STICK_BATTERY_SAMPLE_COUNT) {
        s_battery_sample_count++;
    }
}

static int median_battery_sample(void)
{
    int samples[VIBE_STICK_BATTERY_SAMPLE_COUNT] = {0};
    for (size_t i = 0; i < s_battery_sample_count; ++i) {
        samples[i] = s_battery_samples[i];
    }
    for (size_t i = 1; i < s_battery_sample_count; ++i) {
        int value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    return samples[s_battery_sample_count / 2];
}

static bool battery_drop_hold_active(int64_t now_ms)
{
    return (s_external_power_removed_ms != 0 &&
            now_ms - s_external_power_removed_ms < VIBE_STICK_BATTERY_USB_UNPLUG_HOLD_MS) ||
           (s_deep_sleep_wake_ms != 0 &&
            now_ms - s_deep_sleep_wake_ms < VIBE_STICK_BATTERY_WAKE_STABILIZE_MS);
}

static void update_battery_display_level(int raw_level, int64_t now_ms)
{
    s_battery_raw_level = clamp_percent(raw_level);
    record_battery_sample(s_battery_raw_level);
    int target_level = median_battery_sample();
    if (s_battery_full_latched) {
        target_level = 100;
    }

    if (!s_battery_display_valid) {
        if (s_woke_from_deep_sleep &&
            s_retained_battery_magic == VIBE_STICK_BATTERY_RTC_MAGIC &&
            s_retained_battery_display_level >= 0 &&
            s_retained_battery_display_level <= 100) {
            s_battery_display_level = s_retained_battery_display_level;
        } else {
            s_battery_display_level = target_level;
        }
        s_battery_display_valid = true;
    }
    if (battery_drop_hold_active(now_ms) && target_level < s_battery_display_level) {
        target_level = s_battery_display_level;
    }

    if (target_level > s_battery_display_level) {
        s_battery_display_level++;
    } else if (target_level < s_battery_display_level) {
        s_battery_display_level--;
    }
    s_state.battery = s_battery_display_level;
    s_retained_battery_magic = VIBE_STICK_BATTERY_RTC_MAGIC;
    s_retained_battery_display_level = s_battery_display_level;
}

static void refresh_power_status(bool force_log)
{
    bool was_external_powered = external_powered();
    int64_t now_ms = esp_timer_get_time() / 1000;
    int battery_voltage_mv = -1;
    if (vibe_board_battery_voltage_mv(&battery_voltage_mv) == ESP_OK) {
        s_battery_voltage_mv = battery_voltage_mv;
    }
    int battery_level = 0;
    bool battery_read_ok = false;
    if (vibe_board_battery_level(&battery_level) == ESP_OK) {
        battery_read_ok = true;
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
    if (power_read_ok && was_external_powered && !external_powered()) {
        ESP_LOGI(TAG, "external power removed");
        s_external_power_removed_ms = now_ms;
    }
#if VIBE_BOARD_HOLD_FULL_BATTERY_ICON
    if (!external_powered()) {
        s_battery_full_latched = false;
    } else if (battery_read_ok &&
               battery_level >= VIBE_STICK_BATTERY_FULL_LATCH_PERCENT) {
        s_battery_full_latched = true;
    }
#endif
#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (power_read_ok) {
        update_display_light_sleep_lock(
            !atomic_load(&s_display_rendering_suspended));
    }
#endif
    if (battery_read_ok) {
        update_battery_display_level(battery_level, now_ms);
    }
    static bool last_power_logged = false;
    static bool last_charging = false;
    static bool last_usb_powered = false;
    static int last_logged_raw_level = -1;
    static int last_logged_display_level = -1;
    static int last_logged_voltage_mv = -1;
    bool raw_display_gap = battery_read_ok &&
                           abs(s_battery_raw_level - s_state.battery) >=
                               VIBE_STICK_BATTERY_LOG_GAP_PERCENT;
    if (power_read_ok &&
        (force_log ||
         !last_power_logged ||
         last_charging != s_state.battery_charging ||
         last_usb_powered != s_state.usb_powered ||
         last_logged_display_level != s_state.battery ||
         (raw_display_gap &&
          (last_logged_raw_level != s_battery_raw_level ||
           last_logged_voltage_mv != s_battery_voltage_mv)))) {
        ESP_LOGI(TAG,
                 "power status battery_raw=%d battery_display=%d battery_mv=%d charging=%d usb=%d",
                 s_battery_raw_level, s_state.battery, s_battery_voltage_mv,
                 s_state.battery_charging, s_state.usb_powered);
        last_power_logged = true;
        last_charging = s_state.battery_charging;
        last_usb_powered = s_state.usb_powered;
        last_logged_raw_level = s_battery_raw_level;
        last_logged_display_level = s_state.battery;
        last_logged_voltage_mv = s_battery_voltage_mv;
    }
}

static void maybe_refresh_power_status(int64_t now_ms)
{
    if (s_last_power_status_poll_ms != 0 &&
        now_ms - s_last_power_status_poll_ms < VIBE_STICK_POWER_STATUS_POLL_MS) {
        return;
    }
    s_last_power_status_poll_ms = now_ms;
    refresh_power_status(false);
}

static void poll_state(void)
{
    char response[1536] = {0};
    refresh_power_status(false);
    esp_err_t err = http_request("GET", VIBE_STICK_STATE_PATH, NULL, response, sizeof(response));
    if (err != ESP_OK || response[0] == '\0' || !parse_state_json(response)) {
        provider_display_state_t *display_state = current_provider_display_state();
        strlcpy(display_state->status, "OFFLINE", sizeof(display_state->status));
        s_state.wifi = wifi_connected();
        render_state();
        refresh_bridge_selection_visual();
        return;
    }
    complete_pet_fast_resume();
    render_state();
    refresh_bridge_selection_visual();
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

static void post_recording_playback_event(const char *event_name, esp_err_t playback_err)
{
    char body[192];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"session_id\":\"%s\","
             "\"status\":\"%s\",\"message\":\"%s\"}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             s_recording_session_id,
             playback_err == ESP_OK ? "ok" : "failed",
             esp_err_to_name(playback_err));
    char response[512] = {0};
    esp_err_t err = http_request("POST", VIBE_STICK_EVENT_PATH, body, response, sizeof(response));
    if (err == ESP_OK && response[0] != '\0' && parse_state_json(response)) {
        complete_pet_fast_resume();
        render_state();
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording playback event failed: %s", esp_err_to_name(err));
    }
}

static void clear_ptt_followup_enter_window(void)
{
    s_ptt_followup_session_id[0] = '\0';
    s_ptt_followup_enter_deadline_ms = 0;
}

static void arm_ptt_followup_enter_window(void)
{
    if (s_recording_session_id[0] == '\0') {
        clear_ptt_followup_enter_window();
        return;
    }
    strlcpy(s_ptt_followup_session_id, s_recording_session_id,
            sizeof(s_ptt_followup_session_id));
    s_ptt_followup_enter_deadline_ms = (esp_timer_get_time() / 1000) + PTT_ENTER_GRACE_MS;
}

static bool consume_ptt_followup_enter_window(void)
{
    if (s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK ||
        s_ptt_followup_session_id[0] == '\0' ||
        s_ptt_followup_enter_deadline_ms <= 0) {
        clear_ptt_followup_enter_window();
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms > s_ptt_followup_enter_deadline_ms) {
        clear_ptt_followup_enter_window();
        return false;
    }
    return true;
}

static bool ptt_followup_enter_window_present(void)
{
    return s_ptt_followup_session_id[0] != '\0' &&
           s_ptt_followup_enter_deadline_ms > 0;
}

typedef struct {
    char session_id[40];
    const char *event_name;
    agent_sound_t sound;
} ptt_followup_key_dispatch_t;

static bool post_ptt_followup_key_event(const ptt_followup_key_dispatch_t *dispatch)
{
    if (!dispatch || dispatch->session_id[0] == '\0') {
        return false;
    }

    char body[160];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"session_id\":\"%s\"}",
             dispatch->event_name,
             VIBE_BOARD_EVENT_SOURCE,
             dispatch->session_id);
    char response[512] = {0};
    esp_err_t err = http_request_timeout("POST", VIBE_STICK_EVENT_PATH, body, response,
                                         sizeof(response),
                                         PTT_FOLLOWUP_REQUEST_TIMEOUT_MS);
    bool accepted = false;
    if (err == ESP_OK && response[0] != '\0') {
        cJSON *root = cJSON_Parse(response);
        cJSON *success = root ? cJSON_GetObjectItemCaseSensitive(root, "success") : NULL;
        accepted = cJSON_IsTrue(success);
        cJSON_Delete(root);
    }
    if (accepted) {
        (void)parse_state_json(response);
        complete_pet_fast_resume();
        render_state();
        esp_err_t sound_err = vibe_audio_play_sound(dispatch->sound);
        if (sound_err != ESP_OK) {
            ESP_LOGW(TAG, "follow-up key sound failed: %s", esp_err_to_name(sound_err));
        }
        return true;
    }

    ESP_LOGW(TAG, "PTT follow-up key event rejected err=%s",
             esp_err_to_name(err));
    (void)vibe_audio_play_sound(VIBE_STICK_SOUND_ERROR);
    show_recording_overlay("CONFIRM FAILED", "", true);
    vTaskDelay(pdMS_TO_TICKS(700));
    if (recording_finalize_active() || atomic_load(&s_recording_session_active)) {
        show_recording_overlay("TRANSCRIBING", "", true);
    } else {
        show_recording_overlay(NULL, NULL, false);
    }
    return false;
}

static void ptt_followup_key_dispatch_task(void *arg)
{
    ptt_followup_key_dispatch_t *dispatch = arg;
    (void)post_ptt_followup_key_event(dispatch);
    free(dispatch);
    atomic_store(&s_ptt_followup_dispatch_active, false);
    vTaskDelete(NULL);
}

static bool start_ptt_followup_key_dispatch(const char *event_name, agent_sound_t sound)
{
    if (!event_name || s_ptt_followup_session_id[0] == '\0' ||
        atomic_exchange(&s_ptt_followup_dispatch_active, true)) {
        ESP_LOGW(TAG, "PTT follow-up key dispatch ignored event=%s",
                 event_name ? event_name : "-");
        return false;
    }

    ptt_followup_key_dispatch_t *dispatch = calloc(1, sizeof(*dispatch));
    if (!dispatch) {
        atomic_store(&s_ptt_followup_dispatch_active, false);
        ESP_LOGW(TAG, "PTT follow-up key dispatch allocation failed");
        return false;
    }
    strlcpy(dispatch->session_id, s_ptt_followup_session_id, sizeof(dispatch->session_id));
    dispatch->event_name = event_name;
    dispatch->sound = sound;
    clear_ptt_followup_enter_window();

    BaseType_t ok = xTaskCreatePinnedToCore(ptt_followup_key_dispatch_task,
                                            "ptt_followup", 4096,
                                            dispatch, VIBE_STICK_FOLLOWUP_PRIORITY, NULL,
                                            VIBE_STICK_FOLLOWUP_CORE);
    if (ok != pdPASS) {
        free(dispatch);
        atomic_store(&s_ptt_followup_dispatch_active, false);
        ESP_LOGW(TAG, "PTT follow-up key task create failed");
        return false;
    }
    return true;
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
           strcmp(status, "cyber_failed") == 0 ||
           strcmp(status, "cyber_unconfigured") == 0 ||
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
        set_recording_upload_active(false);
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
    set_recording_upload_active(false);
    s_recording_upload_task = NULL;
    vTaskDelete(NULL);
}

static bool start_recording_upload_task(void)
{
    set_recording_upload_failed(false);
    set_recording_upload_active(true);
    reset_recording_upload_stats();
    s_recording_upload_task = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(recording_upload_task, "recording_upload", 6144,
                                            NULL, 4, &s_recording_upload_task,
                                            VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        set_recording_upload_failed(true);
        set_recording_upload_active(false);
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
    clear_ptt_followup_enter_window();
    clear_cyber_tts_wait();
    if (recording_network_busy() || s_tap_recording_active || s_motion_recording_active) {
        ESP_LOGI(TAG, "recording start ignored while already recording");
        return false;
    }
    generate_recording_session_id(s_recording_session_id, sizeof(s_recording_session_id));
    if (s_recording_session_id[0] == '\0') {
        ESP_LOGW(TAG, "recording start failed: no session id");
        return false;
    }
    set_recording_session_active(true);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    show_recording_overlay("CONNECTING", "", true);

    ESP_LOGI(TAG, "recording start event=%s mode=%s intent=%s session=%s",
             event_name,
             recording_mode_label(),
             recording_mode_intent(),
             s_recording_session_id);
    char body[240];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\","
             "\"audio_source\":\"%s\",\"session_id\":\"%s\","
             "\"intent\":\"%s\",\"mode\":\"%s\"}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             VIBE_BOARD_AUDIO_SOURCE,
             s_recording_session_id,
             recording_mode_intent(),
             recording_mode_label());
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
        show_recording_overlay("CONNECT FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        set_recording_session_active(false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS));
        return false;
    }

    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_RECORDING_START);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording start sound skipped: %s", esp_err_to_name(sound_err));
    }

    esp_err_t audio_err = vibe_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording start failed: %s", esp_err_to_name(audio_err));
        show_recording_overlay("MIC FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        set_recording_session_active(false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS));
        return false;
    }
    if (!start_recording_upload_task()) {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        show_recording_overlay("SEND FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        s_recording_session_id[0] = '\0';
        set_recording_session_active(false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS));
        return false;
    }
    show_recording_overlay("LISTENING", hint, true);
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

    show_recording_overlay("TRANSCRIBING", "", true);
    bool paste_result = !recording_intent_is_cyber();
    ESP_LOGI(TAG, "recording stop event=%s mode=%s intent=%s session=%s paste=%d",
             event_name,
             recording_mode_label(),
             recording_mode_intent(),
             s_recording_session_id,
             paste_result ? 1 : 0);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"paste\":%s,"
             "\"session_id\":\"%s\",\"intent\":\"%s\",\"mode\":\"%s\"}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             paste_result ? "true" : "false",
             s_recording_session_id,
             recording_mode_intent(),
             recording_mode_label());
    char response[1024] = {0};
    esp_err_t err = http_request_timeout("POST", VIBE_STICK_RECORDING_STOP_PATH, body, response,
                                         sizeof(response), RECORDING_STOP_TIMEOUT_MS);
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
            ? "NOT HEARD" : "TRANSCRIBE FAILED";
        show_recording_overlay(title, "", true);
        vTaskDelay(pdMS_TO_TICKS(900));
    } else if (strcmp(recording_status, "cyber_done") == 0) {
        show_recording_overlay("PLAYING", "", true);
        esp_err_t playback_err = play_latest_tts_audio();
        post_recording_playback_event(playback_err == ESP_OK ? "tts_played" : "tts_failed",
                                      playback_err);
    } else if (strcmp(recording_status, "cyber_processing") == 0) {
        start_cyber_tts_wait();
    }
    s_recording_session_id[0] = '\0';
    set_recording_session_active(false);
    s_tap_recording_active = false;
    s_motion_recording_active = false;
    poll_state();
    if (!s_cyber_tts_waiting) {
        show_recording_overlay(NULL, NULL, false);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS));
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
    show_recording_overlay("SENDING", "", true);
    s_tap_recording_active = false;

    if (s_recording_session_id[0] == '\0') {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        clear_ptt_followup_enter_window();
        poll_state();
        show_recording_overlay(NULL, NULL, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS));
        return;
    }

    if (!recording_intent_is_cyber() &&
        (strcmp(event_name, "button_long_stop") == 0 ||
         strcmp(event_name, "button_tap_stop") == 0)) {
        arm_ptt_followup_enter_window();
    } else {
        clear_ptt_followup_enter_window();
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
    ESP_LOGI(TAG, "front tap toggle mode=%s tap_active=%d recording=%d session=%s",
             recording_mode_label(),
             s_tap_recording_active ? 1 : 0,
             vibe_audio_is_recording() ? 1 : 0,
             s_recording_session_id[0] == '\0' ? "-" : s_recording_session_id);
    if (s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK) {
        ESP_LOGI(TAG, "front tap ignored in %s mode", recording_mode_label());
        return;
    }
    if (s_tap_recording_active || vibe_audio_is_recording() || s_recording_session_id[0] != '\0') {
        handle_recording_stop("button_tap_stop");
        s_tap_recording_active = false;
        return;
    }
    s_tap_recording_active = handle_recording_start("button_tap_start", "TAP TO SEND");
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

static uint32_t wifi_reconnect_delay_ms(unsigned int attempt)
{
    static const uint32_t delays_ms[] = {1000, 2000, 4000, 8000, 30000};
    size_t index = attempt;
    if (index >= sizeof(delays_ms) / sizeof(delays_ms[0])) {
        index = sizeof(delays_ms) / sizeof(delays_ms[0]) - 1;
    }
    return delays_ms[index] > VIBE_STICK_WIFI_RECONNECT_MAX_MS
               ? VIBE_STICK_WIFI_RECONNECT_MAX_MS
               : delays_ms[index];
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!wifi_connected()) {
        ESP_LOGI(TAG, "Wi-Fi reconnect attempt=%u", s_wifi_reconnect_attempt);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }
}

static void schedule_wifi_reconnect(void)
{
    if (!s_wifi_reconnect_timer) {
        return;
    }
    uint32_t delay_ms = wifi_reconnect_delay_ms(s_wifi_reconnect_attempt);
    if (s_wifi_reconnect_attempt < UINT32_MAX) {
        s_wifi_reconnect_attempt++;
    }
    if (esp_timer_is_active(s_wifi_reconnect_timer)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_wifi_reconnect_timer));
    }
    ESP_LOGI(TAG, "Wi-Fi reconnect scheduled in %u ms", (unsigned int)delay_ms);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(s_wifi_reconnect_timer, (uint64_t)delay_ms * 1000));
}

static void request_wifi_reconnect_now(void)
{
    if (wifi_connected() || !s_wifi_reconnect_timer ||
        !esp_timer_is_active(s_wifi_reconnect_timer)) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_wifi_reconnect_timer));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(s_wifi_reconnect_timer, 1));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_reconnect_attempt = 0;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        set_wifi_connected(false);
        s_state.wifi_ip[0] = '\0';
        if (atomic_load(&s_deep_sleep_committed)) {
            ESP_LOGI(TAG, "Wi-Fi stopped for deep sleep reason=%d",
                     disconnected ? disconnected->reason : -1);
            return;
        }
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
        schedule_wifi_reconnect();
        render_state();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        set_wifi_connected(true);
        if (got_ip) {
            snprintf(s_state.wifi_ip, sizeof(s_state.wifi_ip),
                     IPSTR, IP2STR(&got_ip->ip_info.ip));
        }
        s_wifi_profile_retry_count = 0;
        s_wifi_reconnect_attempt = 0;
        if (s_wifi_reconnect_timer &&
            esp_timer_is_active(s_wifi_reconnect_timer)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_wifi_reconnect_timer));
        }
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
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_timer_cb,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&reconnect_timer_args, &s_wifi_reconnect_timer),
        TAG, "wifi reconnect timer");

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
#if VIBE_BOARD_WIFI_MAX_TX_POWER > 0
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_max_tx_power(VIBE_BOARD_WIFI_MAX_TX_POWER),
        TAG, "wifi tx power");
    ESP_LOGI(TAG, "Wi-Fi max TX power set to %.2f dBm",
             VIBE_BOARD_WIFI_MAX_TX_POWER / 4.0);
#endif
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(VIBE_STICK_WIFI_IDLE_PS), TAG, "wifi power save");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");
    return ESP_OK;
}

#if VIBE_STICK_ANIM_PREVIEW
static void request_anim_preview_switch(const char *source)
{
    ESP_LOGI(TAG, "anim preview switch requested source=%s", source);
    s_anim_switch_requested = true;
}
#endif

static void button_press_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_front_button_iot_down_ms = now_ms;
    const int64_t entry_deadline =
        atomic_load(&s_bridge_selection_entry_deadline_ms);
    const bool entry_open =
        entry_deadline > 0 && now_ms <= entry_deadline &&
        !recording_network_busy();
    if (atomic_load(&s_bridge_selection_active) || entry_open) {
        if (!atomic_load(&s_bridge_selection_active)) {
            atomic_store(&s_bridge_selection_active, true);
            atomic_store(&s_bridge_selection_confirming, false);
            atomic_store(&s_bridge_selection_entry_deadline_ms, 0);
            s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_SELECTING;
            ESP_LOGI(TAG, "bridge selection mode entered");
        }
        atomic_store(&s_front_bridge_gesture_active, true);
        atomic_store(&s_front_bridge_gesture_confirmed, false);
        clear_ptt_followup_enter_window();
        ESP_LOGI(TAG, "front button reserved for bridge selection");
        return;
    }
    ESP_LOGI(TAG, "front button down mode=%s", recording_mode_label());
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        s_anim_press_down_switch_handled = true;
        request_anim_preview_switch("down");
    }
#endif
}

static void button_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_front_button_iot_single_ms = now_ms;
    if (atomic_load(&s_bridge_selection_active) ||
        atomic_load(&s_front_bridge_gesture_active) ||
        now_ms <= atomic_load(&s_front_bridge_click_suppress_until_ms)) {
        ESP_LOGI(TAG, "front single click consumed by bridge selection");
        return;
    }
    ESP_LOGI(TAG, "front single click mode=%s", recording_mode_label());
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        if (s_anim_press_down_switch_handled) {
            s_anim_press_down_switch_handled = false;
            return;
        }
        request_anim_preview_switch("single");
        return;
    }
#endif
    if (recording_intent_is_cyber()) {
        clear_ptt_followup_enter_window();
    } else if (consume_ptt_followup_enter_window()) {
        if (!start_ptt_followup_key_dispatch("button_followup_enter",
                                             VIBE_STICK_SOUND_FOLLOWUP_ENTER)) {
            clear_ptt_followup_enter_window();
        }
        return;
    } else if (ptt_followup_enter_window_present() || recording_finalize_active()) {
        ESP_LOGI(TAG, "front single click ignored after dictation stop");
        return;
    }
    queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);
}

static void button_double_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (atomic_load(&s_bridge_selection_active) ||
        atomic_load(&s_front_bridge_gesture_active) ||
        now_ms <= atomic_load(&s_front_bridge_click_suppress_until_ms)) {
        ESP_LOGI(TAG, "front double click consumed by bridge selection");
        return;
    }
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        return;
    }
#endif
    if (consume_ptt_followup_enter_window()) {
        if (!start_ptt_followup_key_dispatch("button_followup_escape",
                                             VIBE_STICK_SOUND_FOLLOWUP_ESCAPE)) {
            clear_ptt_followup_enter_window();
        }
        return;
    } else if (ptt_followup_enter_window_present() || recording_finalize_active()) {
        ESP_LOGI(TAG, "front double click ignored after dictation stop");
        return;
    }
    queue_event(VIBE_STICK_EVENT_DOUBLE_CLICK);
}

static void side_button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    s_side_button_mode_hold_reached = true;
    ESP_LOGI(TAG, "side button mode hold reached");
}

static void side_button_calibration_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    s_side_button_calibration_hold_reached = true;
    ESP_LOGI(TAG, "side button calibration hold reached");
}

static void side_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (s_side_button_calibration_hold_reached) {
        s_side_button_calibration_hold_reached = false;
        s_side_button_mode_hold_reached = false;
        if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
            queue_event(VIBE_STICK_EVENT_MOTION_CALIBRATE);
        } else {
            queue_event(VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE);
        }
        return;
    }
    if (s_side_button_mode_hold_reached) {
        s_side_button_mode_hold_reached = false;
        queue_event(VIBE_STICK_EVENT_RECORDING_MODE_TOGGLE);
        return;
    }
    const bool can_arm = !recording_network_busy();
    atomic_store(&s_bridge_selection_entry_deadline_ms,
                 can_arm ? esp_timer_get_time() / 1000 +
                               BRIDGE_SELECTION_ENTRY_WINDOW_MS
                         : 0);
    ESP_LOGI(TAG, "side button release: full bridge scan entry_window=%d",
             can_arm ? 1 : 0);
    queue_event(VIBE_STICK_EVENT_BRIDGE_SCAN_FULL);
}

static void button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (atomic_load(&s_bridge_selection_active) ||
        atomic_load(&s_front_bridge_gesture_active)) {
        ESP_LOGI(TAG, "front PTT long press consumed by bridge selection");
        return;
    }
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        return;
    }
#endif
    if (consume_ptt_followup_enter_window()) {
        s_long_press_active = false;
        if (!start_ptt_followup_key_dispatch("button_followup_enter",
                                             VIBE_STICK_SOUND_FOLLOWUP_ENTER)) {
            clear_ptt_followup_enter_window();
        }
        ESP_LOGI(TAG, "front long press accepted as dictation confirmation");
        return;
    } else if (ptt_followup_enter_window_present() || recording_finalize_active()) {
        s_long_press_active = false;
        ESP_LOGI(TAG, "front long press ignored after dictation stop");
        return;
    }
    if (s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK) {
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

static void bridge_selection_confirm_long_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (!atomic_load(&s_bridge_selection_active) ||
        !atomic_load(&s_front_bridge_gesture_active) ||
        atomic_exchange(&s_bridge_selection_confirming, true)) {
        return;
    }
    atomic_store(&s_front_bridge_gesture_confirmed, true);
    ESP_LOGI(TAG, "bridge selection confirm hold");
    queue_bridge_control(BRIDGE_CONTROL_CONFIRM);
}

static void button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_front_button_iot_up_ms = now_ms;
    if (atomic_load(&s_front_bridge_gesture_active)) {
        if (!atomic_load(&s_front_bridge_gesture_confirmed) &&
            !atomic_load(&s_bridge_selection_confirming)) {
            queue_bridge_control(BRIDGE_CONTROL_NEXT);
        }
        atomic_store(&s_front_bridge_click_suppress_until_ms,
                     now_ms + BRIDGE_SELECTION_CLICK_SUPPRESS_MS);
        atomic_store(&s_front_bridge_gesture_active, false);
        s_long_press_active = false;
        s_wake_front_button_pending = false;
        ESP_LOGI(TAG, "front button release consumed by bridge selection");
        return;
    }
#if VIBE_STICK_ANIM_PREVIEW
    if (recording_animation_preview_active()) {
        s_long_press_active = false;
        s_wake_front_button_pending = false;
        return;
    }
#endif
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
        .disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &gpio_config, &button), TAG, "button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, button_press_down_cb, NULL),
                        TAG, "button down");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, button_single_click_cb, NULL),
                        TAG, "button single");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, button_double_click_cb, NULL),
                        TAG, "button double");
    button_event_args_t front_long_press_args = {
        .long_press = {
            .press_time = FRONT_PTT_LONG_PRESS_MS,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_START, &front_long_press_args, button_long_start_cb, NULL),
                        TAG, "button long");
    button_event_args_t bridge_confirm_args = {
        .long_press = {
            .press_time = BRIDGE_SELECTION_CONFIRM_HOLD_MS,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_LONG_PRESS_START,
                                               &bridge_confirm_args,
                                               bridge_selection_confirm_long_cb,
                                               NULL),
                        TAG, "button bridge confirm");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, button_up_cb, NULL),
                        TAG, "button up");

    const button_gpio_config_t side_gpio_config = {
        .gpio_num = VIBE_BOARD_PIN_BUTTON_SIDE,
        .active_level = 0,
        .enable_power_save = true,
        .disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_config, &side_gpio_config, &side_button), TAG, "side button");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(side_button, BUTTON_PRESS_UP, NULL,
                                               side_button_up_cb, NULL),
                        TAG, "side button release");
    button_event_args_t side_long_press_args = {
        .long_press = {
            .press_time = SIDE_MODE_TOGGLE_HOLD_MS,
        },
    };
    ESP_RETURN_ON_ERROR(iot_button_register_cb(side_button, BUTTON_LONG_PRESS_START, &side_long_press_args,
                                               side_button_long_start_cb, NULL),
                        TAG, "side button long");
    button_event_args_t side_calibration_press_args = {
        .long_press = {
            .press_time = SIDE_MANUAL_CALIBRATION_HOLD_MS,
        },
    };
    ESP_RETURN_ON_ERROR(
        iot_button_register_cb(side_button, BUTTON_LONG_PRESS_START,
                               &side_calibration_press_args,
                               side_button_calibration_long_start_cb, NULL),
        TAG, "side button calibration");
    return ESP_OK;
}

static void capture_deep_sleep_front_button_intent(void)
{
    if (!s_woke_from_deep_sleep ||
        s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK) {
        return;
    }
    if (!front_button_is_pressed()) {
        return;
    }
    s_wake_front_button_pending = true;
    ESP_LOGI(TAG, "front button held during deep sleep wake; pending PTT restore");
}

static void capture_deep_sleep_motion_intent(void)
{
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (!s_woke_from_deep_sleep ||
        s_boot_wake_cause != ESP_SLEEP_WAKEUP_EXT1 ||
        (s_boot_ext1_wake_status &
         (1ULL << VIBE_BOARD_PIN_MOTION_WAKE)) == 0) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_motion_clear_wake_status());
    if (s_recording_trigger_mode != RECORDING_TRIGGER_LIFT_TO_TALK ||
        s_motion_calibrating) {
        ESP_LOGW(TAG, "motion wake ignored because LIFT calibration is unavailable");
        return;
    }
    s_motion_wake_confirm_pending = true;
    s_motion_wake_confirm_deadline_ms =
        esp_timer_get_time() / 1000 + VIBE_STICK_MOTION_WAKE_CONFIRM_MS;
    s_motion_lift_armed = false;
    s_motion_start_pending = false;
    s_motion_wake_network_pending = false;
    s_motion_wake_network_deadline_ms = 0;
    ESP_LOGI(TAG, "motion deep-sleep wake pending %dms posture confirmation",
             VIBE_STICK_MOTION_WAKE_CONFIRM_MS);
#endif
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
    if (s_recording_trigger_mode != RECORDING_TRIGGER_PUSH_TO_TALK) {
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

static void bridge_control_task(void *arg)
{
    (void)arg;
    bridge_control_command_t command;
    while (true) {
        if (xQueueReceive(s_bridge_control_queue, &command, portMAX_DELAY) !=
            pdTRUE) {
            continue;
        }
        if (command == BRIDGE_CONTROL_NEXT) {
            if (atomic_load(&s_bridge_selection_active) &&
                !atomic_load(&s_bridge_selection_confirming)) {
                cycle_bridge_profile();
            }
        } else if (command == BRIDGE_CONTROL_CONFIRM &&
                   atomic_load(&s_bridge_selection_active)) {
            begin_bridge_selection_confirmation();
        }
    }
}

static void maybe_timeout_motion_calibration(int64_t now_ms)
{
    if (!s_motion_calibrating ||
        s_motion_calibration_deadline_ms == 0 ||
        now_ms < s_motion_calibration_deadline_ms) {
        return;
    }

    if (s_motion_calibration_had_previous &&
        vibe_motion_apply_calibration(&s_motion_previous_calibration) == ESP_OK) {
        s_motion_calibrating = false;
        s_motion_calibration_deadline_ms = 0;
        s_motion_calibration_had_previous = false;
        s_motion_lift_armed = true;
        s_motion_start_pending = false;
        ESP_LOGW(TAG, "lift calibration timed out; restored previous calibration");
        render_state();
        show_mode_switch_visual("CAL FAILED", "LIFT RESTORED",
                                s_mode_switch_dict_frames,
                                sizeof(s_mode_switch_dict_frames) /
                                    sizeof(s_mode_switch_dict_frames[0]),
                                lv_color_hex(0xfacc15));
        return;
    }

    ESP_LOGW(TAG, "lift calibration timed out without fallback; returning to PTT");
    set_push_to_talk_trigger_mode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
    render_state();
    show_mode_switch_visual("CAL FAILED", "PTT",
                            s_mode_switch_dict_frames,
                            sizeof(s_mode_switch_dict_frames) /
                                sizeof(s_mode_switch_dict_frames[0]),
                            lv_color_hex(0xfca5a5));
}

static uint32_t state_poll_interval_ms(int64_t now_ms)
{
    return s_last_activity_ms != 0 &&
                   now_ms - s_last_activity_ms <
                       VIBE_STICK_STATE_POLL_INTERACTIVE_MS
               ? VIBE_STICK_STATE_POLL_MS
               : VIBE_STICK_STATE_POLL_IDLE_MS;
}

static uint32_t app_task_wait_ms(void)
{
    if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK ||
        (s_display_power_state == DISPLAY_POWER_ACTIVE &&
         s_current_backlight != LCD_BACKLIGHT_DEFAULT) ||
        (s_display_power_state == DISPLAY_POWER_DIMMED &&
         s_current_backlight != LCD_BACKLIGHT_IDLE) ||
        (s_display_power_state == DISPLAY_POWER_OFF &&
         s_current_backlight != LCD_BACKLIGHT_OFF)) {
        return VIBE_STICK_APP_MOTION_WAIT_MS;
    }
    return VIBE_STICK_APP_IDLE_WAIT_MS;
}

static void app_task(void *arg)
{
    (void)arg;
    agent_event_t event;
    int64_t last_poll = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        maybe_refresh_power_status(now_ms);
        maybe_timeout_cyber_tts_wait(now_ms);
        maybe_timeout_motion_calibration(now_ms);
        update_power_saving(now_ms);
        maybe_enter_deep_sleep(now_ms);
        handle_deep_sleep_front_button_intent();
        poll_front_button_fallback(now_ms);
        const bool network_busy = recording_network_busy();
        if (s_display_power_state == DISPLAY_POWER_ACTIVE &&
            wifi_connected() && !network_busy &&
            now_ms - last_poll >= state_poll_interval_ms(now_ms)) {
            last_poll = now_ms;
            poll_state();
        }
        uint32_t ota_interval_ms =
            external_powered() ? OTA_PERIODIC_CHECK_MS : OTA_BATTERY_CHECK_MS;
        bool ota_power_policy_allows =
            external_powered() ||
            s_display_power_state == DISPLAY_POWER_ACTIVE;
        if (ota_power_policy_allows &&
            wifi_connected() && !network_busy && !ota_in_progress() &&
            now_ms - s_last_ota_check_ms >= ota_interval_ms) {
            s_last_ota_check_ms = now_ms;
            queue_event(VIBE_STICK_EVENT_OTA_CHECK);
        }
        if (s_motion_wake_network_pending &&
            now_ms >= s_motion_wake_network_deadline_ms) {
            ESP_LOGW(TAG, "motion lift cancelled: Wi-Fi did not connect within %dms",
                     VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS);
            s_motion_wake_network_pending = false;
            s_motion_wake_network_deadline_ms = 0;
            s_motion_start_pending = false;
            s_motion_lift_armed = true;
            show_recording_overlay("CONNECT FAILED", "", true);
            vTaskDelay(pdMS_TO_TICKS(700));
            show_recording_overlay(NULL, NULL, false);
        }
        if (vibe_motion_available() &&
            s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
            vibe_motion_event_t motion_event = vibe_motion_poll(now_ms);
            if (s_motion_calibrating && !vibe_motion_is_calibrating()) {
                s_motion_calibrating = false;
                s_motion_calibration_deadline_ms = 0;
                s_motion_calibration_had_previous = false;
                s_motion_lift_armed = true;
                s_motion_start_pending = false;
                ESP_ERROR_CHECK_WITHOUT_ABORT(save_motion_calibration());
                ESP_LOGI(TAG, "lift recording mode calibration complete");
                render_state();
            }
            bool motion_wake_handled = false;
            if (!s_motion_calibrating && s_motion_wake_confirm_pending) {
                motion_wake_handled = true;
                if (now_ms >= s_motion_wake_confirm_deadline_ms) {
                    s_motion_wake_confirm_pending = false;
                    s_motion_wake_confirm_deadline_ms = 0;
                    if (vibe_motion_is_lifted()) {
                        ESP_LOGI(TAG, "motion wake confirmed lifted; starting recording");
                        request_motion_recording_start();
                    } else {
                        s_motion_lift_armed = true;
                        s_motion_false_wake_sleep_deadline_ms =
                            now_ms + VIBE_STICK_MOTION_FALSE_WAKE_DISPLAY_MS;
                        ESP_LOGI(TAG, "motion wake rejected; display remains on for %dms",
                                 VIBE_STICK_MOTION_FALSE_WAKE_DISPLAY_MS);
                    }
                }
            }
            if (!motion_wake_handled &&
                !s_motion_calibrating &&
                motion_event == VIBE_MOTION_EVENT_FLAT) {
                if (s_motion_start_pending) {
                    ESP_LOGI(TAG, "motion lift start deferred request cancelled by flat posture");
                    s_motion_start_pending = false;
                }
                if (s_motion_wake_network_pending) {
                    ESP_LOGI(TAG, "motion lift Wi-Fi wait cancelled by flat posture");
                    s_motion_wake_network_pending = false;
                    s_motion_wake_network_deadline_ms = 0;
                    show_recording_overlay(NULL, NULL, false);
                }
                if (s_motion_recording_active) {
                    (void)queue_event(VIBE_STICK_EVENT_MOTION_STOP);
                } else if (!s_motion_lift_armed) {
                    s_motion_lift_armed = true;
                }
            } else if (!motion_wake_handled &&
                       !s_motion_calibrating && s_motion_lift_armed &&
                       motion_event == VIBE_MOTION_EVENT_LIFTED &&
                       !s_motion_recording_active) {
                request_motion_recording_start();
            } else if (!motion_wake_handled &&
                       !s_motion_calibrating && s_motion_start_pending &&
                       !s_motion_recording_active) {
                request_motion_recording_start();
            }
        }
        if (xQueueReceive(s_event_queue, &event,
                          pdMS_TO_TICKS(app_task_wait_ms())) != pdTRUE) {
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
            if (!handle_recording_start("button_long_start", "RELEASE TO SEND")) {
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
        case VIBE_STICK_EVENT_RECORDING_INTENT_TOGGLE:
            toggle_recording_intent();
            break;
        case VIBE_STICK_EVENT_MOTION_CALIBRATE:
            start_manual_motion_calibration();
            break;
        case VIBE_STICK_EVENT_BRIDGE_SCAN_FULL:
            (void)start_bridge_discovery_task(true);
            break;
        case VIBE_STICK_EVENT_MOTION_START:
            if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK &&
                !s_motion_recording_active) {
                s_motion_start_pending = false;
                s_motion_recording_active =
                    handle_recording_start("motion_lift_start", "PLACE DOWN");
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
        case VIBE_STICK_EVENT_TTS_PROBE: {
            clear_cyber_tts_wait();
            show_recording_overlay("PLAYING", "", true);
            esp_err_t playback_err = play_latest_tts_audio();
            post_recording_playback_event(playback_err == ESP_OK ? "tts_probe_played" : "tts_probe_failed",
                                          playback_err);
            show_recording_overlay(NULL, NULL, false);
            break;
        }
        case VIBE_STICK_EVENT_OTA_CHECK:
            start_ota_check_task();
            break;
        }
    }
}

#if VIBE_STICK_SERIAL_DEBUG_ENABLED
static void serial_debug_task(void *arg)
{
    (void)arg;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_driver_config_t usb_config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usb_err = usb_serial_jtag_driver_install(&usb_config);
    if (usb_err != ESP_OK && usb_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "serial debug USB-JTAG init failed: %s",
                 esp_err_to_name(usb_err));
    } else {
        ESP_LOGI(TAG, "serial debug USB-JTAG input ready");
    }
#endif
    while (true) {
        uint8_t input = 0;
        bool received = false;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        received =
            usb_serial_jtag_read_bytes(&input, 1, pdMS_TO_TICKS(20)) == 1;
#else
        received = esp_rom_output_rx_one_char(&input) == 0;
#endif
        if (!received) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (input == 's' || input == 'S') {
            ESP_LOGI(TAG, "serial debug command: side button full scan");
            side_button_up_cb(NULL, NULL);
        } else if (input == 'c' || input == 'C') {
            ESP_LOGI(TAG, "serial debug command: clear runtime bridge profiles");
            bridge_profiles_clear();
        } else if (input == 'p' || input == 'P') {
            ESP_LOGI(TAG, "serial debug command: front button short press");
            button_press_down_cb(NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(80));
            button_up_cb(NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            button_single_click_cb(NULL, NULL);
        } else if (input == 'h' || input == 'H') {
            ESP_LOGI(TAG, "serial debug command: front button 1.5s hold");
            button_press_down_cb(NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(FRONT_PTT_LONG_PRESS_MS));
            button_long_start_cb(NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(
                BRIDGE_SELECTION_CONFIRM_HOLD_MS -
                FRONT_PTT_LONG_PRESS_MS));
            bridge_selection_confirm_long_cb(NULL, NULL);
            button_up_cb(NULL, NULL);
        }
    }
}
#endif

static esp_err_t init_power_management(void)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    ESP_RETURN_ON_ERROR(esp_pm_configure(&config), TAG, "power management");
    ESP_LOGI(TAG, "power management max=%dMHz min=%dMHz light_sleep=%d",
             config.max_freq_mhz, config.min_freq_mhz,
             config.light_sleep_enable ? 1 : 0);
#if VIBE_BOARD_HAS_GPIO_BACKLIGHT
    ESP_RETURN_ON_ERROR(
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display_active",
                           &s_display_no_light_sleep_lock),
        TAG, "display light sleep lock");
    ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(s_display_no_light_sleep_lock),
                        TAG, "hold display light sleep lock");
    s_display_no_light_sleep_lock_held = true;
    ESP_LOGI(TAG, "automatic light sleep blocked while display is active");
#endif
#endif
    return ESP_OK;
}

void app_main(void)
{
    s_boot_wake_cause = esp_sleep_get_wakeup_cause();
    s_boot_reset_reason = esp_reset_reason();
    s_boot_ext1_wake_status = esp_sleep_get_ext1_wakeup_status();
    if (s_retained_boot_magic != VIBE_STICK_RETAINED_BOOT_MAGIC) {
        s_retained_boot_magic = VIBE_STICK_RETAINED_BOOT_MAGIC;
        s_retained_boot_count = 0;
    }
    s_retained_boot_count++;
    s_woke_from_deep_sleep =
        s_boot_wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED;
    if (s_woke_from_deep_sleep) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            rtc_gpio_deinit(VIBE_BOARD_PIN_BUTTON_FRONT));
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
        if ((s_boot_ext1_wake_status &
             (1ULL << VIBE_BOARD_PIN_MOTION_WAKE)) != 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                rtc_gpio_deinit(VIBE_BOARD_PIN_MOTION_WAKE));
        }
#endif
    }
    if (s_woke_from_deep_sleep) {
        s_deep_sleep_wake_ms = esp_timer_get_time() / 1000;
        s_pet_fast_resume_pending = true;
        s_pet_animation_resume_ms = s_deep_sleep_wake_ms + VIBE_STICK_PET_FAST_RESUME_MAX_MS;
    }
    ESP_LOGI(TAG, "boot %s board=%s version=%s build=%s transport=%s",
             FIRMWARE_NAME, VIBE_BOARD_NAME, FIRMWARE_VERSION, FIRMWARE_BUILD_ID, TRANSPORT);
    ESP_LOGI(TAG, "battery curve=%s", VIBE_BOARD_BATTERY_CURVE_VERSION);
    ESP_LOGI(TAG,
             "boot diagnostics wake=%s(%d) ext1=0x%llx reset=%s(%d) boot_count=%lu",
             wake_cause_label(s_boot_wake_cause), (int)s_boot_wake_cause,
             (unsigned long long)s_boot_ext1_wake_status,
             reset_reason_label(s_boot_reset_reason), (int)s_boot_reset_reason,
             (unsigned long)s_retained_boot_count);
    ESP_ERROR_CHECK(init_power_management());
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(load_deep_sleep_record());

    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_init_power());
    s_boot_power_status = vibe_board_boot_power_status();
    s_event_queue = xQueueCreate(16, sizeof(agent_event_t));
    s_bridge_control_queue =
        xQueueCreate(8, sizeof(bridge_control_command_t));
    ESP_ERROR_CHECK(s_event_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_bridge_control_queue ? ESP_OK : ESP_ERR_NO_MEM);
    s_lvgl_lock = xSemaphoreCreateMutex();
    s_bridge_target_lock = xSemaphoreCreateMutex();
    s_bridge_profiles_lock = xSemaphoreCreateMutex();
    s_bridge_probe_lock = xSemaphoreCreateMutex();
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
    xTaskCreate(serial_debug_task, "serial_debug", 6144, NULL, 2, NULL);
#endif
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
    if (motion_err == ESP_OK) {
        esp_err_t calibration_err = load_motion_calibration();
        if (calibration_err != ESP_OK &&
            calibration_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "motion calibration load skipped: %s",
                     esp_err_to_name(calibration_err));
        }
    }
    set_push_to_talk_trigger_mode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(restore_recording_mode_preference());
    render_state();
    BaseType_t bridge_control_ok =
        xTaskCreatePinnedToCore(bridge_control_task, "bridge_control", 4096,
                                NULL, 5, NULL, VIBE_STICK_APP_CORE);
    ESP_ERROR_CHECK(bridge_control_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(init_button());
    capture_deep_sleep_front_button_intent();
    capture_deep_sleep_motion_intent();
    ESP_ERROR_CHECK(vibe_audio_init());
    xTaskCreatePinnedToCore(app_task, "agent_app", 6144, NULL, 4, NULL,
                            VIBE_STICK_APP_CORE);
}
