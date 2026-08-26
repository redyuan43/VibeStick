#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "vibe_audio.h"
#include "vibe_app_runtime.h"
#include "vibe_app_command.h"
#include "vibe_app_state.h"
#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "vibe_bridge_client.h"
#include "vibe_bridge_profile_policy.h"
#include "vibe_bridge_registry.h"
#include "vibe_cardputer_runtime.h"
#include "vibe_input.h"
#include "vibe_input_router.h"
#include "vibe_keyboard.h"
#include "vibe_motion.h"
#include "vibe_motion_controller.h"
#include "vibe_ota_policy.h"
#include "vibe_ota_runtime.h"
#include "vibe_power_policy.h"
#include "vibe_power_runtime.h"
#include "vibe_recording_policy.h"
#include "vibe_recording_controller.h"
#include "vibe_recording_upload.h"
#include "vibe_settings.h"
#include "vibe_state_json.h"
#include "vibe_stick_config.h"
#include "vibe_ui.h"
#include "vibe_wifi_policy.h"
#include "vibe_wifi_runtime.h"
#include "vibe_wav.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
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
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "soc/soc_caps.h"

#define LCD_BACKLIGHT_DEFAULT VIBE_BOARD_LCD_BACKLIGHT_DEFAULT
#define LCD_BACKLIGHT_IDLE VIBE_BOARD_LCD_BACKLIGHT_IDLE
#define LCD_BACKLIGHT_OFF VIBE_BOARD_LCD_BACKLIGHT_OFF
#define RECORDING_UPLOAD_BATCH_CHUNKS 4
#define RECORDING_UPLOAD_BUFFER_BYTES 8192
#define RECORDING_UPLOAD_PARALLEL_WORKERS 1
#define RECORDING_UPLOAD_HTTP_TIMEOUT_MS 5000
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
#define HTTP_CLIENT_RX_BUFFER_SIZE 2048
#define HTTP_CLIENT_TX_BUFFER_SIZE 2048
#define BRIDGE_HEALTH_RESPONSE_BYTES 512
#define TTS_AUDIO_MAX_BYTES (1024 * 1024)
#define FIRMWARE_BUILD_ID VIBE_BOARD_NAME "-v" FIRMWARE_VERSION
#define VIBE_STICK_APP_CORE 0
#define VIBE_STICK_NETWORK_CORE 1
#define VIBE_STICK_FOLLOWUP_CORE VIBE_STICK_APP_CORE
#define VIBE_STICK_FOLLOWUP_PRIORITY 6
#define VIBE_STICK_IDLE_DIM_MS 30000
#define VIBE_STICK_IDLE_OFF_MS 60000
#define VIBE_STICK_DEEP_SLEEP_RETRY_MS 5000
#define VIBE_STICK_DEEP_SLEEP_DIAGNOSTIC_REPORT_MS 30000
#define VIBE_STICK_SETTINGS_TIMEOUT_MS 30000
#define CARD_OPT_CLICK_WINDOW_MS 180
#define CARD_KEYBOARD_REPORT_QUEUE_LENGTH 64
#define CARD_KEYBOARD_REPORT_TIMEOUT_MS 700
#define CARD_KEYBOARD_HEARTBEAT_MS 250
#define CARD_POINTER_REPORT_TIMEOUT_MS 700
#define CARD_POINTER_HEARTBEAT_MS 250
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
#define VIBE_STICK_CYBER_TTS_WAIT_TIMEOUT_MS 180000
#define VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS 15000
#define VIBE_STICK_MOTION_WAKE_CONFIRM_MS 500
#define VIBE_STICK_MOTION_FALSE_WAKE_DISPLAY_MS 3000
#define SIDE_MODE_TOGGLE_HOLD_MS 3000
#define SIDE_MANUAL_CALIBRATION_HOLD_MS 6000
#define VIBE_STICK_STATE_POLL_IDLE_MS 10000
#if defined(VIBE_BOARD_CARDPUTER_ADV)
#define VIBE_STICK_STATE_POLL_INTERACTIVE_MS 0
#else
#define VIBE_STICK_STATE_POLL_INTERACTIVE_MS 15000
#endif
#define VIBE_STICK_APP_IDLE_WAIT_MS 1000
#define VIBE_STICK_APP_MOTION_WAIT_MS 20
#define VIBE_STICK_WIFI_IDLE_PS WIFI_PS_NONE
#define VIBE_STICK_WIFI_RECONNECT_MAX_MS 30000
#define VIBE_STICK_ANIM_PREVIEW 0
#ifndef VIBE_STICK_SERIAL_DEBUG_ENABLED
#define VIBE_STICK_SERIAL_DEBUG_ENABLED 0
#endif
#ifndef VIBE_STICK_RECORDING_DIAGNOSTICS_ENABLED
#define VIBE_STICK_RECORDING_DIAGNOSTICS_ENABLED 0
#endif
#if VIBE_STICK_ANIM_PREVIEW
#define VIBE_STICK_OTA_ENABLED 0
#else
#define VIBE_STICK_OTA_ENABLED 1
#endif
#define RECORDING_RSSI_UNKNOWN -127
#define DEVICE_COMMAND_POLL_TIMEOUT_MS 25000
#define DEVICE_COMMAND_RETRY_DELAY_MS 1000
#define CARDPUTER_COMMAND_POLL_INTERVAL_MS 5000
#define WIFI_PROFILE_SSID_LEN VIBE_WIFI_PROFILE_SSID_LEN
#define WIFI_PROFILE_PASSWORD_LEN VIBE_WIFI_PROFILE_PASSWORD_LEN
#define DEVICE_PREF_NAMESPACE "vibe_prefs"
#define DEVICE_PREF_RECORDING_MODE_KEY "rec_mode"
#define DEVICE_PREF_RECORDING_TRIGGER_KEY "rec_trig"
#define DEVICE_PREF_RECORDING_INTENT_KEY "rec_intent"
#define DEVICE_PREF_SLEEP_MINUTES_KEY "sleep_min"
#define DEVICE_PREF_RECORDING_DIAGNOSTIC_KEY "rec_diag"
#define RECORDING_DIAGNOSTIC_MAGIC 0x56444244u
#define RECORDING_DIAGNOSTIC_STORE_VERSION 2u
#define DEVICE_PREF_MOTION_CALIBRATION_KEY "motion_cal_v1"
#define MOTION_CALIBRATION_MAGIC 0x564d4341u
#define MOTION_CALIBRATION_STORE_VERSION 1
#define DEEP_SLEEP_NAMESPACE "vibe_sleep"
#define DEEP_SLEEP_RECORD_KEY "last_entry"
#define DEEP_SLEEP_RECORD_MAGIC 0x56534c50u
#define DEEP_SLEEP_RECORD_VERSION 1
#define BRIDGE_TARGET_HOST_LEN VIBE_BRIDGE_PROFILE_HOST_LEN
#define BRIDGE_TARGET_PROFILE_LEN VIBE_BRIDGE_PROFILE_ID_LEN
#define BRIDGE_TARGET_LABEL_LEN VIBE_BRIDGE_PROFILE_LABEL_LEN
#define BRIDGE_TARGET_TOKEN_LEN VIBE_BRIDGE_PROFILE_TOKEN_LEN
#define CARD_SETUP_MANUAL_BRIDGE_ID "cardputer-manual"
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

#ifndef VIBE_STICK_WIFI_PROFILES
#define VIBE_STICK_WIFI_PROFILES \
    { { VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD } }
#endif

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static const vibe_wifi_profile_t k_configured_wifi_profiles[] = {
    {VIBE_STICK_CARDPUTER_WIFI_SSID, VIBE_STICK_CARDPUTER_WIFI_PASSWORD},
};
#else
static const vibe_wifi_profile_t k_configured_wifi_profiles[] = VIBE_STICK_WIFI_PROFILES;
#endif

#ifndef VIBE_STICK_BRIDGE_PROFILES
#define VIBE_STICK_BRIDGE_PROFILES \
    { { VIBE_STICK_BRIDGE_ID, VIBE_STICK_BRIDGE_LABEL, VIBE_STICK_BRIDGE_HOST, \
        VIBE_STICK_BRIDGE_PORT, VIBE_STICK_BRIDGE_TOKEN } }
#endif

#if defined(VIBE_BOARD_CARDPUTER_ADV)
#define VIBE_STICK_DEFAULT_BRIDGE_ID "lan-192-168-100-142"
#define VIBE_STICK_DEFAULT_BRIDGE_HOST "192.168.100.142"
#define VIBE_STICK_DEFAULT_BRIDGE_PORT 8765
static const bridge_profile_config_t k_configured_bridge_profiles[] = {
    {VIBE_STICK_DEFAULT_BRIDGE_ID, VIBE_STICK_BRIDGE_LABEL,
     VIBE_STICK_DEFAULT_BRIDGE_HOST, VIBE_STICK_DEFAULT_BRIDGE_PORT, ""},
};
#else
#define VIBE_STICK_DEFAULT_BRIDGE_ID VIBE_STICK_BRIDGE_ID
#define VIBE_STICK_DEFAULT_BRIDGE_HOST VIBE_STICK_BRIDGE_HOST
#define VIBE_STICK_DEFAULT_BRIDGE_PORT VIBE_STICK_BRIDGE_PORT
static const bridge_profile_config_t k_configured_bridge_profiles[] =
    VIBE_STICK_BRIDGE_PROFILES;
#endif
_Static_assert(sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]) > 0,
               "at least one bridge profile is required");
_Static_assert(sizeof(k_configured_bridge_profiles) / sizeof(k_configured_bridge_profiles[0]) <=
                   VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT,
               "too many bridge profiles");
_Static_assert(VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT ==
                   VIBE_BRIDGE_REGISTRY_MAX_PROFILES,
               "bridge profile capacities must match");

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

typedef struct {
    uint32_t samples;
    int64_t target_total_ms;
    int64_t target_max_ms;
    int64_t init_total_ms;
    int64_t init_max_ms;
    int64_t connect_total_ms;
    int64_t connect_max_ms;
    int64_t headers_total_ms;
    int64_t headers_max_ms;
    int64_t response_total_ms;
    int64_t response_max_ms;
} recording_http_phase_stats_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char board[16];
    vibe_recording_upload_diagnostics_t diagnostics;
    recording_http_phase_stats_t http;
} recording_diagnostic_store_t;

typedef enum {
    BRIDGE_CONTROL_NEXT,
    BRIDGE_CONTROL_CONFIRM,
} bridge_control_command_t;

typedef struct {
    vibe_provider_id_t id;
    const char *key;
    const char *display_name;
    bool enabled;
    bool implemented;
} agent_provider_config_t;

typedef struct {
    char *data;
    int capacity;
    int used;
    int64_t connected_ms;
    int64_t headers_sent_ms;
    int64_t first_response_ms;
} http_response_capture_t;

typedef vibe_bridge_target_t bridge_target_t;

typedef enum {
    RECORDING_MODE_PUSH_TO_TALK,
    RECORDING_MODE_LIFT_TO_TALK,
    RECORDING_MODE_ANIMATION_PREVIEW,
    RECORDING_MODE_CYBER_FORTUNE,
    RECORDING_MODE_CYBER_ALMANAC,
} legacy_recording_mode_t;

typedef enum {
    BRIDGE_SELECTION_UI_IDLE,
    BRIDGE_SELECTION_UI_SELECTING,
    BRIDGE_SELECTION_UI_CONFIRMING,
    BRIDGE_SELECTION_UI_CONFIRMED,
} bridge_selection_ui_phase_t;

#if defined(VIBE_BOARD_CARDPUTER_ADV)
typedef enum {
    CARD_SETUP_WIFI_SSID = 0,
    CARD_SETUP_WIFI_PASSWORD,
    CARD_SETUP_BRIDGE_HOST,
    CARD_SETUP_BRIDGE_PORT,
    CARD_SETUP_BRIDGE_TOKEN,
    CARD_SETUP_FIELD_COUNT,
} card_setup_field_t;

typedef struct {
    char wifi_ssid[WIFI_PROFILE_SSID_LEN];
    char wifi_password[WIFI_PROFILE_PASSWORD_LEN];
    char bridge_host[BRIDGE_TARGET_HOST_LEN];
    char bridge_port[12];
    char bridge_token[BRIDGE_TARGET_TOKEN_LEN];
} card_setup_draft_t;

typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t key_count;
} card_keyboard_report_t;

#endif

static QueueHandle_t s_event_queue;
static QueueHandle_t s_bridge_control_queue;
static vibe_wifi_runtime_t s_wifi;
static vibe_bridge_registry_t s_bridge_registry;
static vibe_bridge_client_t s_bridge_client;
static vibe_ui_t s_ui;
static atomic_bool s_deep_sleep_committed;
static recording_http_phase_stats_t s_recording_http_stats;
static bridge_discovered_profile_t
    s_bridge_scan_profiles[VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT];
static size_t s_bridge_scan_profile_count;
static TaskHandle_t s_bridge_discovery_task;
static atomic_bool s_bridge_discovery_active;
static atomic_bool s_bridge_selection_active;
static atomic_bool s_bridge_selection_confirming;
static atomic_bool s_front_bridge_gesture_active;
static atomic_bool s_front_bridge_gesture_confirmed;
static atomic_int_fast64_t s_bridge_selection_entry_deadline_ms;
static atomic_int_fast64_t s_front_bridge_click_suppress_until_ms;
static bool s_recording_overlay_visible;
static atomic_bool s_settings_active;
static atomic_int_fast64_t s_settings_deadline_ms;
static vibe_settings_page_t s_settings_page = VIBE_SETTINGS_PAGE_MODE;
static recording_trigger_mode_t s_settings_draft_trigger =
    RECORDING_TRIGGER_PUSH_TO_TALK;
static uint8_t s_sleep_minutes = VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES;
static uint8_t s_settings_draft_sleep_minutes =
    VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES;
static bool s_long_press_active;
static vibe_motion_calibration_t s_motion_previous_calibration;
static vibe_motion_controller_t s_motion_controller;

#define s_motion_calibrating s_motion_controller.state.calibrating
#define s_motion_calibration_deadline_ms \
    s_motion_controller.state.calibration_deadline_ms
#define s_motion_calibration_had_previous \
    s_motion_controller.state.calibration_had_previous
#define s_motion_lift_armed s_motion_controller.state.lift_armed
#define s_motion_arm_prompt_visible \
    s_motion_controller.state.arm_prompt_visible
#define s_motion_start_pending s_motion_controller.state.start_pending
#define s_motion_wake_confirm_pending \
    s_motion_controller.state.wake_confirm_pending
#define s_motion_wake_confirm_deadline_ms \
    s_motion_controller.state.wake_confirm_deadline_ms
#define s_motion_wake_network_pending \
    s_motion_controller.state.wake_network_pending
#define s_motion_wake_network_deadline_ms \
    s_motion_controller.state.wake_network_deadline_ms
#define s_motion_false_wake_sleep_deadline_ms \
    s_motion_controller.state.false_wake_sleep_deadline_ms
static char s_last_alert_event_id[56];
static char s_last_alert_type[24];
static bool s_alert_sound_baseline_ready;
static uint32_t s_device_command_cursor;
static TaskHandle_t s_recording_finalize_task;
static char s_recording_finalize_event_name[32];
static atomic_bool s_ptt_followup_dispatch_active;
static vibe_recording_controller_t s_recording;

#define s_recording_trigger_mode s_recording.trigger_mode
#define s_recording_intent s_recording.intent
#define s_recording_session_id s_recording.session_id
#define s_recording_chunk_id s_recording.chunk_id
#define s_recording_uploaded_bytes s_recording.uploaded_bytes
#define s_recording_local_capture s_recording.local_capture
#define s_recording_bridge_stop_required s_recording.bridge_stop_required
#define s_recording_upload_abort_requested s_recording.upload_abort_requested
#define s_recording_session_active s_recording.session_active
#define s_recording_finalize_active s_recording.finalize_active
#define s_tap_recording_active s_recording.tap_active
#define s_motion_recording_active s_recording.motion_active
#define s_ptt_followup s_recording.followup

static bool s_cyber_tts_waiting;
static int64_t s_cyber_tts_wait_deadline_ms;
static vibe_ota_runtime_t s_ota;
static int64_t s_next_deep_sleep_attempt_ms;
static int64_t s_last_deep_sleep_diagnostic_report_ms;
static const char *s_deep_sleep_block_reason = "boot";
static vibe_power_runtime_t s_power;

#define s_last_activity_ms s_power.state.last_activity_ms
#define s_last_backlight_fade_ms s_power.state.last_backlight_fade_ms
#define s_last_power_status_poll_ms \
    s_power.state.last_power_status_poll_ms
#define s_current_backlight s_power.state.current_backlight
#define s_display_power_state s_power.state.display_state
#define s_deep_sleep_wake_ms s_power.state.deep_sleep_wake_ms
#define s_external_power_removed_ms \
    s_power.state.external_power_removed_ms
#define s_battery_display_valid s_power.state.battery_display_valid
#define s_battery_full_latched s_power.state.battery_full_latched
#define s_battery_display_level s_power.state.battery_display_level
#define s_battery_raw_level s_power.state.battery_raw_level
#define s_battery_voltage_mv s_power.state.battery_voltage_mv
static bridge_selection_ui_phase_t s_bridge_selection_ui_phase;
static int64_t s_bridge_selection_ui_deadline_ms;
#if VIBE_STICK_ANIM_PREVIEW
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
static bool s_front_status_led_pressed;
static bool s_status_led_on;

#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
static esp_pm_lock_handle_t s_display_no_light_sleep_lock;
static bool s_display_no_light_sleep_lock_held;
#endif
static bool s_ui_ready;
static bool s_woke_from_deep_sleep;
static esp_sleep_wakeup_cause_t s_boot_wake_cause;
static esp_reset_reason_t s_boot_reset_reason;
static uint64_t s_boot_ext1_wake_status;
static vibe_board_boot_power_status_t s_boot_power_status;
static bool s_wake_front_button_pending;
static bool s_pet_fast_resume_pending;
static int64_t s_pet_animation_resume_ms;
static deep_sleep_record_t s_last_deep_sleep_record;
static bool s_last_deep_sleep_record_valid;
RTC_DATA_ATTR static uint32_t s_retained_battery_magic;
RTC_DATA_ATTR static int s_retained_battery_display_level = -1;
RTC_DATA_ATTR static uint32_t s_retained_boot_magic;
RTC_DATA_ATTR static uint32_t s_retained_boot_count;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
static bool s_card_setup_active;
static card_setup_field_t s_card_setup_field;
static card_setup_draft_t s_card_setup_draft;
static char s_card_setup_error[48];
static QueueHandle_t s_card_keyboard_report_queue;
static uint8_t s_card_host_usages[4][14];
static uint8_t s_card_host_modifiers;
static bool s_card_local_consumed[4][14];
static vibe_cardputer_runtime_t s_card_runtime;
#endif

static bool wifi_connected(void)
{
    return vibe_wifi_runtime_connected(&s_wifi);
}

static bool ota_in_progress(void)
{
    return atomic_load(&s_ota.active);
}

static bool recording_finalize_active(void)
{
    return atomic_load(&s_recording_finalize_active);
}

static void set_recording_finalize_active(bool active)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_FINALIZE_ACTIVE,
        .data.flag = active,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
}

static bool settings_active(void)
{
    return atomic_load(&s_settings_active);
}

static void update_status_led(void)
{
    const bool enabled =
        s_front_status_led_pressed || atomic_load(&s_recording_session_active);
    if (enabled == s_status_led_on) {
        return;
    }
    esp_err_t err = vibe_board_status_led_set(enabled);
    if (err == ESP_OK) {
        s_status_led_on = enabled;
    } else {
        ESP_LOGW(TAG, "status LED update failed enabled=%d: %s",
                 enabled ? 1 : 0, esp_err_to_name(err));
    }
}

static bool recording_network_busy(void)
{
    return vibe_audio_is_recording() ||
           atomic_load(&s_recording_session_active) ||
           vibe_recording_upload_active() ||
           recording_finalize_active();
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static bool card_opt_busy(void)
{
    return vibe_cardputer_runtime_opt_busy(&s_card_runtime) ||
           recording_network_busy();
}

static bool card_message_busy(void)
{
    return card_opt_busy() || ota_in_progress();
}
#endif

static vibe_app_state_t s_state;

static const agent_provider_config_t s_provider_configs[] = {
    {
        .id = VIBE_PROVIDER_CODEX,
        .key = "codex",
        .display_name = "Codex",
        .enabled = true,
        .implemented = true,
    },
    {
        .id = VIBE_PROVIDER_CLAUDE,
        .key = "claude",
        .display_name = "Claude",
        .enabled = true,
        .implemented = true,
    },
};

static void render_state(void);
static void bridge_client_set_headers(esp_http_client_handle_t client,
                                      const char *token,
                                      void *context);
static void register_activity(void);
static bool external_powered(void);
static bool display_should_stay_active(void);
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
static void post_deep_sleep_diagnostic(const char *reason);
static void recording_set_motion_active(bool active);
static uint32_t recording_crc32(const uint8_t *data, size_t len);
#if defined(VIBE_BOARD_CARDPUTER_ADV)
static void card_setup_enter(void);
static void card_keyboard_event(const vibe_key_event_t *event, void *context);
#endif

static bool queue_event(vibe_app_command_t type)
{
    if (!s_event_queue) {
        return false;
    }
    vibe_app_event_t event = {.type = type};
    return xQueueSend(s_event_queue, &event, 0) == pdTRUE;
}

static bool queue_input_signal(vibe_input_signal_t signal)
{
    const vibe_app_command_t command =
        vibe_input_router_command(signal);
    return command != VIBE_APP_COMMAND_NONE && queue_event(command);
}

static bool queue_bridge_control(bridge_control_command_t command)
{
    return s_bridge_control_queue &&
           xQueueSend(s_bridge_control_queue, &command, 0) == pdTRUE;
}

static const agent_provider_config_t *provider_config(vibe_provider_id_t provider)
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
    return provider_config(s_state.current_provider);
}

static uint32_t current_provider_accent_rgb(void)
{
    return s_state.current_provider == VIBE_PROVIDER_CLAUDE
               ? 0xd97757
               : 0x4d82ff;
}

static vibe_provider_state_t *current_provider_display_state(void)
{
    return vibe_app_state_current_provider(&s_state);
}

static void switch_provider(void)
{
    if (s_recording_overlay_visible) {
        ESP_LOGI(TAG, "provider switch ignored while overlay is visible");
        return;
    }
    (void)vibe_app_state_select_provider(
        &s_state,
        vibe_app_state_next_provider(s_state.current_provider),
        true);
    const agent_provider_config_t *provider = current_provider_config();
    ESP_LOGI(TAG, "provider switched to %s", provider->key);
    render_state();
}

static const char *recording_mode_label(void)
{
    return vibe_recording_controller_trigger_label(
        &s_recording, s_motion_calibrating);
}

static void sanitize_recording_intent(void)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_INTENT,
        .data.intent = s_recording_intent,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
}

static const char *recording_intent_label(void)
{
    return vibe_recording_controller_intent_label(&s_recording);
}

static bool recording_intent_is_cyber(void)
{
    return vibe_recording_controller_is_cyber(&s_recording);
}

static const char *recording_mode_intent(void)
{
    return vibe_recording_controller_intent_key(&s_recording);
}

static void show_trigger_mode_switch_visual(void);
static void show_recording_intent_switch_visual(void);

#if VIBE_STICK_ANIM_PREVIEW
static bool recording_animation_preview_active(void)
{
    return false;
}
#endif

static void set_motion_arm_prompt(bool visible)
{
    if (!s_ui_ready || s_motion_arm_prompt_visible == visible) {
        return;
    }
    const vibe_motion_controller_command_t command = {
        .type = VIBE_MOTION_COMMAND_SET_ARM_PROMPT,
        .data.flag = visible,
    };
    (void)vibe_motion_controller_handle(
        &s_motion_controller, &command);
    show_recording_overlay(visible ? "PLACE FLAT" : NULL,
                           visible ? "TO ARM LIFT" : NULL,
                           visible);
}

static void reset_recording_trigger_runtime_state(void)
{
    recording_set_motion_active(false);
    const vibe_motion_controller_command_t command = {
        .type = VIBE_MOTION_COMMAND_RESET,
    };
    (void)vibe_motion_controller_handle(
        &s_motion_controller, &command);
    memset(&s_motion_previous_calibration, 0, sizeof(s_motion_previous_calibration));
}

static void set_push_to_talk_trigger_mode(void)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_TRIGGER,
        .data.trigger = RECORDING_TRIGGER_PUSH_TO_TALK,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
    reset_recording_trigger_runtime_state();
    set_motion_arm_prompt(false);
    if (vibe_motion_available()) {
        esp_err_t err = vibe_motion_suspend();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PTT mode IMU suspend failed: %s",
                     esp_err_to_name(err));
        }
    }
}

#if VIBE_STICK_RECORDING_DIAGNOSTICS_ENABLED
static void persist_recording_diagnostic(void)
{
    recording_diagnostic_store_t store = {
        .magic = RECORDING_DIAGNOSTIC_MAGIC,
        .version = RECORDING_DIAGNOSTIC_STORE_VERSION,
        .size = sizeof(recording_diagnostic_store_t),
        .http = s_recording_http_stats,
    };
    snprintf(store.board, sizeof(store.board), "%s", VIBE_BOARD_NAME);
    vibe_recording_upload_diagnostics(&store.diagnostics);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, DEVICE_PREF_RECORDING_DIAGNOSTIC_KEY,
                           &store, sizeof(store));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle) {
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording diagnostic save failed: %s",
                 esp_err_to_name(err));
    }
}

static void log_persisted_recording_diagnostic(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    recording_diagnostic_store_t store = {0};
    size_t size = sizeof(store);
    err = nvs_get_blob(handle, DEVICE_PREF_RECORDING_DIAGNOSTIC_KEY,
                       &store, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(store) ||
        store.magic != RECORDING_DIAGNOSTIC_MAGIC ||
        store.version != RECORDING_DIAGNOSTIC_STORE_VERSION ||
        store.size != sizeof(store) ||
        strncmp(store.board, VIBE_BOARD_NAME, sizeof(store.board)) != 0) {
        return;
    }

    const vibe_audio_stats_t *audio = &store.diagnostics.audio;
    const vibe_recording_upload_stats_t *upload = &store.diagnostics.upload;
    const recording_http_phase_stats_t *http = &store.http;
    const int64_t samples = http->samples > 0 ? http->samples : 1;
    ESP_LOGI(TAG,
             "saved recording diagnostic board=%s read=%u queued=%u dropped=%u "
             "drop_bytes=%u posts=%u uploaded=%u upload_fail=%u max_pending=%u "
             "post_ms_avg=%lld post_ms_max=%lld rssi=%d/%d "
             "http_samples=%u target_ms=%lld/%lld init_ms=%lld/%lld "
             "connect_ms=%lld/%lld headers_ms=%lld/%lld response_ms=%lld/%lld",
             store.board,
             (unsigned)audio->chunks_read,
             (unsigned)audio->chunks_queued,
             (unsigned)audio->chunks_dropped,
             (unsigned)audio->bytes_dropped,
             (unsigned)upload->upload_posts,
             (unsigned)upload->uploaded_bytes,
             (unsigned)upload->upload_failures,
             (unsigned)upload->max_pending_chunks,
             (long long)vibe_recording_upload_stats_average_post_ms(upload),
             (long long)upload->post_duration_max_ms,
             upload->start_rssi,
             upload->stop_rssi,
             (unsigned)http->samples,
             (long long)(http->target_total_ms / samples),
             (long long)http->target_max_ms,
             (long long)(http->init_total_ms / samples),
             (long long)http->init_max_ms,
             (long long)(http->connect_total_ms / samples),
             (long long)http->connect_max_ms,
             (long long)(http->headers_total_ms / samples),
             (long long)http->headers_max_ms,
             (long long)(http->response_total_ms / samples),
             (long long)http->response_max_ms);
}
#else
static void persist_recording_diagnostic(void)
{
}

static void log_persisted_recording_diagnostic(void)
{
}
#endif

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
    const vibe_motion_controller_command_t command = {
        .type = VIBE_MOTION_COMMAND_BEGIN_CALIBRATION,
        .data.calibration = {
            .now_ms = esp_timer_get_time() / 1000,
            .had_previous = s_motion_calibration_had_previous,
        },
    };
    (void)vibe_motion_controller_handle(
        &s_motion_controller, &command);
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
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_TRIGGER,
        .data.trigger = RECORDING_TRIGGER_LIFT_TO_TALK,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
    vibe_motion_calibration_t calibration = {0};
    if (vibe_motion_get_calibration(&calibration) != ESP_OK) {
        err = begin_motion_calibration(reason);
        if (err != ESP_OK) {
            set_push_to_talk_trigger_mode();
            return err;
        }
        return ESP_OK;
    }
    const vibe_motion_controller_command_t ready_command = {
        .type = VIBE_MOTION_COMMAND_SET_LIFT_READY,
    };
    (void)vibe_motion_controller_handle(
        &s_motion_controller, &ready_command);
    set_motion_arm_prompt(true);
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
        err = nvs_set_u8(handle, DEVICE_PREF_SLEEP_MINUTES_KEY,
                         s_sleep_minutes);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "write recording mode preference");
    ESP_LOGI(TAG, "saved device preferences trigger=%s intent=%s sleep=%umin",
             recording_mode_label(), recording_intent_label(),
             (unsigned)s_sleep_minutes);
    return ESP_OK;
}

static esp_err_t restore_sleep_preference(void)
{
    s_sleep_minutes = VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEVICE_PREF_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open sleep preference NVS");

    uint8_t stored_minutes = 0;
    err = nvs_get_u8(handle, DEVICE_PREF_SLEEP_MINUTES_KEY, &stored_minutes);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read sleep preference");
    if (!vibe_settings_sleep_minutes_valid(stored_minutes)) {
        ESP_LOGW(TAG, "stored sleep preference rejected value=%u; using %umin",
                 (unsigned)stored_minutes,
                 VIBE_SETTINGS_DEFAULT_SLEEP_MINUTES);
        return ESP_OK;
    }
    s_sleep_minutes = stored_minutes;
    ESP_LOGI(TAG, "restored sleep preference=%umin", (unsigned)s_sleep_minutes);
    return ESP_OK;
}

static void migrate_legacy_recording_mode(uint8_t stored_mode)
{
    vibe_recording_command_t intent_command = {
        .type = VIBE_RECORDING_COMMAND_SET_INTENT,
    };
    switch ((legacy_recording_mode_t)stored_mode) {
    case RECORDING_MODE_LIFT_TO_TALK:
        intent_command.data.intent = RECORDING_INTENT_DICTATION;
        (void)vibe_recording_controller_handle(
            &s_recording, &intent_command);
        if (set_lift_to_talk_trigger_mode("stored") != ESP_OK) {
            set_push_to_talk_trigger_mode();
        }
        break;
    case RECORDING_MODE_CYBER_FORTUNE:
        intent_command.data.intent = RECORDING_INTENT_CYBER_FORTUNE;
        (void)vibe_recording_controller_handle(
            &s_recording, &intent_command);
        set_push_to_talk_trigger_mode();
        break;
    case RECORDING_MODE_CYBER_ALMANAC:
        intent_command.data.intent = RECORDING_INTENT_CYBER_ALMANAC;
        (void)vibe_recording_controller_handle(
            &s_recording, &intent_command);
        set_push_to_talk_trigger_mode();
        break;
    case RECORDING_MODE_PUSH_TO_TALK:
    case RECORDING_MODE_ANIMATION_PREVIEW:
    default:
        intent_command.data.intent = RECORDING_INTENT_DICTATION;
        (void)vibe_recording_controller_handle(
            &s_recording, &intent_command);
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
            const vibe_recording_command_t command = {
                .type = VIBE_RECORDING_COMMAND_SET_INTENT,
                .data.intent = (recording_intent_t)stored_intent,
            };
            (void)vibe_recording_controller_handle(
                &s_recording, &command);
        } else {
            const vibe_recording_command_t command = {
                .type = VIBE_RECORDING_COMMAND_SET_INTENT,
                .data.intent = RECORDING_INTENT_DICTATION,
            };
            (void)vibe_recording_controller_handle(
                &s_recording, &command);
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
        const vibe_recording_command_t command = {
            .type = VIBE_RECORDING_COMMAND_SET_INTENT,
            .data.intent = RECORDING_INTENT_DICTATION,
        };
        (void)vibe_recording_controller_handle(
            &s_recording, &command);
        clear_ptt_followup_enter_window();
        ESP_LOGI(TAG, "recording intent switch ignored: cyber intents unavailable on %s",
                 VIBE_BOARD_NAME);
        ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
        render_state();
        show_recording_intent_switch_visual();
        return;
    }
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_CYCLE_INTENT,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
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

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static void lvgl_lock(void)
{
    vibe_ui_lock(&s_ui);
}

static void lvgl_unlock(void)
{
    vibe_ui_unlock(&s_ui);
}
#endif

static void ui_brightness_changed(uint8_t brightness, void *context)
{
    (void)context;
    const vibe_power_runtime_command_t command = {
        .type = VIBE_POWER_COMMAND_SET_BACKLIGHT,
        .data.brightness = brightness,
    };
    (void)vibe_power_runtime_handle(&s_power, &command);
}

static void set_backlight(uint8_t brightness)
{
    vibe_ui_set_backlight(&s_ui, brightness);
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
        board_requires_light_sleep_lock() || display_active ||
        external_powered();
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
#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (!suspended) {
        update_display_light_sleep_lock(true);
    }
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_ui_set_rendering_suspended(&s_ui, suspended));
#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (suspended) {
        update_display_light_sleep_lock(false);
    }
#endif
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
           settings_active() ||
           s_recording_overlay_visible ||
           recording_network_busy() ||
           s_tap_recording_active ||
           s_motion_recording_active ||
           s_motion_calibrating ||
           s_motion_wake_confirm_pending ||
           s_motion_wake_network_pending;
    return active_work;
}

static bool power_active_work(void *context)
{
    (void)context;
    return display_should_stay_active();
}

static bool power_false_wake_due(void *context)
{
    (void)context;
    return s_motion_controller.state.false_wake_due;
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
    return vibe_input_front_pressed();
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
    if (settings_active() || !recording_intent_is_cyber() ||
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
        queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_SINGLE);
    }
}

static void register_activity(void)
{
    const vibe_power_runtime_command_t command = {
        .type = VIBE_POWER_COMMAND_ACTIVITY,
        .data.time_ms = esp_timer_get_time() / 1000,
    };
    (void)vibe_power_runtime_handle(&s_power, &command);
    s_next_deep_sleep_attempt_ms = 0;
    s_motion_false_wake_sleep_deadline_ms = 0;
    request_wifi_reconnect_now();
    if (vibe_ui_rendering_suspended(&s_ui)) {
        set_display_rendering_suspended(false);
    }
    vibe_ui_register_activity(&s_ui);
    if (s_display_power_state != DISPLAY_POWER_ACTIVE ||
        s_current_backlight != LCD_BACKLIGHT_DEFAULT) {
        set_backlight(LCD_BACKLIGHT_DEFAULT);
    }
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static void card_message_activity(void)
{
    const vibe_power_runtime_command_t command = {
        .type = VIBE_POWER_COMMAND_ACTIVITY,
        .data.time_ms = esp_timer_get_time() / 1000,
    };
    (void)vibe_power_runtime_handle(&s_power, &command);
    s_next_deep_sleep_attempt_ms = 0;
    s_motion_false_wake_sleep_deadline_ms = 0;
    request_wifi_reconnect_now();
    if (vibe_ui_rendering_suspended(&s_ui)) {
        set_display_rendering_suspended(false);
    }
    vibe_ui_register_activity(&s_ui);
    if (s_display_power_state != DISPLAY_POWER_ACTIVE ||
        s_current_backlight != LCD_BACKLIGHT_DEFAULT) {
        set_backlight(LCD_BACKLIGHT_DEFAULT);
    }
}

static esp_err_t card_message_set_landscape(bool landscape)
{
    return vibe_ui_set_landscape(&s_ui, landscape);
}
#endif

static void update_power_saving(int64_t now_ms)
{
    vibe_power_runtime_tick(&s_power, now_ms);
    const bool false_wake_sleep_due =
        s_motion_controller.state.false_wake_due;
    const vibe_power_display_state_t next_state =
        s_power.state.display_state;
    uint8_t target = LCD_BACKLIGHT_DEFAULT;
    if (next_state == DISPLAY_POWER_DIMMED) {
        target = LCD_BACKLIGHT_IDLE;
    } else if (next_state == DISPLAY_POWER_OFF) {
        target = LCD_BACKLIGHT_OFF;
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
}

static void request_motion_recording_start(void)
{
    s_motion_false_wake_sleep_deadline_ms = 0;
    set_motion_arm_prompt(false);
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
    if (queue_input_signal(VIBE_INPUT_SIGNAL_MOTION_LIFTED)) {
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

static gpio_num_t sleep_button_wake_gpio(void)
{
#if VIBE_BOARD_HAS_FRONT_BUTTON
    return VIBE_BOARD_PIN_BUTTON_FRONT;
#elif VIBE_BOARD_HAS_SIDE_BUTTON
    return VIBE_BOARD_PIN_BUTTON_SIDE;
#else
    return GPIO_NUM_NC;
#endif
}

static uint64_t sleep_button_wake_mask(void)
{
    const gpio_num_t gpio = sleep_button_wake_gpio();
    return gpio == GPIO_NUM_NC ? 0 : 1ULL << gpio;
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static uint64_t cardputer_keyboard_wake_mask(void)
{
    return 1ULL << VIBE_BOARD_PIN_KEYBOARD_INT;
}
#endif

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
#if VIBE_BOARD_HAS_FRONT_BUTTON
    if ((wake_mask & (1ULL << VIBE_BOARD_PIN_BUTTON_FRONT)) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_BUTTON_FRONT),
                            TAG, "front wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_BUTTON_FRONT),
                            TAG, "front wake pull-down");
    }
#endif
#if !VIBE_BOARD_HAS_FRONT_BUTTON && VIBE_BOARD_HAS_SIDE_BUTTON
    if ((wake_mask & (1ULL << VIBE_BOARD_PIN_BUTTON_SIDE)) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_BUTTON_SIDE),
                            TAG, "side wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_BUTTON_SIDE),
                            TAG, "side wake pull-down");
    }
#endif
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if ((wake_mask & (1ULL << VIBE_BOARD_PIN_MOTION_WAKE)) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_MOTION_WAKE),
                            TAG, "motion wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_MOTION_WAKE),
                            TAG, "motion wake pull-down");
    }
#endif
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    if ((wake_mask & cardputer_keyboard_wake_mask()) != 0) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(VIBE_BOARD_PIN_KEYBOARD_INT),
                            TAG, "keyboard wake pull-up");
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(VIBE_BOARD_PIN_KEYBOARD_INT),
                            TAG, "keyboard wake pull-down");
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
    gpio_num_t ext0_gpio = sleep_button_wake_gpio();
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    const uint64_t keyboard_wake_mask = cardputer_keyboard_wake_mask();
    wake_mask |= keyboard_wake_mask;
#else
    const uint64_t keyboard_wake_mask = 0;
#endif
    uint64_t ext1_wake_mask = keyboard_wake_mask;
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    const bool motion_wake_enabled =
        s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK &&
        VIBE_BOARD_PIN_MOTION_WAKE != GPIO_NUM_NC;
#else
    const bool motion_wake_enabled = false;
#endif
    if (sleep_wake_gpio_is_active(ext0_gpio)) {
        s_deep_sleep_block_reason = "front_gpio_low";
        ESP_LOGW(TAG, "deep sleep skipped: ext0 wake gpio=%d is already active",
                 (int)ext0_gpio);
        return false;
    }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    if (sleep_wake_gpio_is_active(VIBE_BOARD_PIN_KEYBOARD_INT)) {
        s_deep_sleep_block_reason = "keyboard_gpio_low";
        ESP_LOGW(TAG, "deep sleep skipped: keyboard wake gpio=%d is already active",
                 (int)VIBE_BOARD_PIN_KEYBOARD_INT);
        return false;
    }
#endif

    if (!prepare_imu_deep_sleep_wake(&wake_mask)) {
        s_deep_sleep_block_reason = "imu_prepare";
        return false;
    }
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (motion_wake_enabled && !wait_for_motion_wake_idle()) {
        s_deep_sleep_block_reason = "motion_gpio_unsettled";
        cancel_imu_deep_sleep_wake();
        return false;
    }
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32)
    esp_err_t pull_err = configure_deep_sleep_button_pullups(wake_mask);
    if (pull_err != ESP_OK) {
        s_deep_sleep_block_reason = "wake_pullup";
        ESP_LOGW(TAG, "deep sleep skipped: wake pull-up setup failed: %s",
                 esp_err_to_name(pull_err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
#endif
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (motion_wake_enabled &&
        sleep_wake_gpio_is_active(VIBE_BOARD_PIN_MOTION_WAKE)) {
        s_deep_sleep_block_reason = "motion_gpio_low";
        ESP_LOGW(TAG, "deep sleep skipped: motion wake gpio=%d is already active",
                 (int)VIBE_BOARD_PIN_MOTION_WAKE);
        cancel_imu_deep_sleep_wake();
        return false;
    }
#endif

    esp_err_t err = save_deep_sleep_record(wake_mask);
    if (err != ESP_OK) {
        s_deep_sleep_block_reason = "nvs_record";
        ESP_LOGW(TAG, "deep sleep skipped: persistent record failed: %s",
                 esp_err_to_name(err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    err = vibe_audio_prepare_deep_sleep();
    if (err != ESP_OK) {
        s_deep_sleep_block_reason = "audio_prepare";
        ESP_LOGW(TAG, "deep sleep skipped: audio power-down failed: %s",
                 esp_err_to_name(err));
        cancel_imu_deep_sleep_wake();
        return false;
    }
    set_display_rendering_suspended(true);
    set_backlight(LCD_BACKLIGHT_OFF);

    s_front_status_led_pressed = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_status_led_set(false));
    s_status_led_on = false;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    vibe_cardputer_runtime_stop_interactive(&s_card_runtime);
#endif
    ESP_LOGI(TAG,
             "entering deep sleep board=%s mode=%s timeout=%umin wake_mask=0x%llx",
             VIBE_BOARD_NAME, recording_mode_label(), (unsigned)s_sleep_minutes,
             (unsigned long long)wake_mask);
    atomic_store(&s_deep_sleep_committed, true);
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_wifi_runtime_stop_for_sleep(&s_wifi));
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
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
    if (motion_wake_enabled) {
        ext1_wake_mask |= 1ULL << VIBE_BOARD_PIN_MOTION_WAKE;
    }
#else
    (void)motion_wake_enabled;
#endif
    if (ext1_wake_mask != 0) {
#if defined(CONFIG_IDF_TARGET_ESP32)
        err = esp_sleep_enable_ext1_wakeup_io(
            ext1_wake_mask, ESP_EXT1_WAKEUP_ALL_LOW);
#else
        err = esp_sleep_enable_ext1_wakeup_io(
            ext1_wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ext1 wake source setup failed; restarting instead of sleeping: %s",
                     esp_err_to_name(err));
            esp_restart();
        }
    }
    esp_deep_sleep_start();
    return true;
}

static void maybe_enter_deep_sleep(int64_t now_ms)
{
#if !VIBE_BOARD_HAS_AUTOMATIC_DEEP_SLEEP
    (void)now_ms;
    return;
#else
    if (s_sleep_minutes == VIBE_SETTINGS_SLEEP_DISABLED_MINUTES) {
        return;
    }
    const bool false_wake_sleep_due =
        s_motion_false_wake_sleep_deadline_ms != 0 &&
        now_ms >= s_motion_false_wake_sleep_deadline_ms;
    if (s_last_activity_ms == 0) {
        return;
    }
    if (!false_wake_sleep_due &&
        (now_ms - s_last_activity_ms) <
            vibe_settings_sleep_timeout_ms(s_sleep_minutes)) {
        return;
    }
    if (deep_sleep_should_stay_awake()) {
        if (display_should_stay_active()) {
            s_deep_sleep_block_reason = "active_work";
        } else if (s_motion_start_pending) {
            s_deep_sleep_block_reason = "motion_start_pending";
        } else if (ota_in_progress()) {
            s_deep_sleep_block_reason = "ota_in_progress";
        } else {
            s_deep_sleep_block_reason = "stay_awake";
        }
        if (now_ms - s_last_deep_sleep_diagnostic_report_ms >=
            VIBE_STICK_DEEP_SLEEP_DIAGNOSTIC_REPORT_MS) {
            s_last_deep_sleep_diagnostic_report_ms = now_ms;
            post_deep_sleep_diagnostic(s_deep_sleep_block_reason);
        }
        return;
    }
    if (s_current_backlight != LCD_BACKLIGHT_OFF) {
        s_deep_sleep_block_reason = "backlight_not_off";
        if (now_ms - s_last_deep_sleep_diagnostic_report_ms >=
            VIBE_STICK_DEEP_SLEEP_DIAGNOSTIC_REPORT_MS) {
            s_last_deep_sleep_diagnostic_report_ms = now_ms;
            post_deep_sleep_diagnostic(s_deep_sleep_block_reason);
        }
        return;
    }
    if (s_next_deep_sleep_attempt_ms != 0 &&
        now_ms < s_next_deep_sleep_attempt_ms) {
        return;
    }
    if (!enter_deep_sleep()) {
        s_next_deep_sleep_attempt_ms =
            now_ms + VIBE_STICK_DEEP_SLEEP_RETRY_MS;
        if (now_ms - s_last_deep_sleep_diagnostic_report_ms >=
            VIBE_STICK_DEEP_SLEEP_DIAGNOSTIC_REPORT_MS) {
            s_last_deep_sleep_diagnostic_report_ms = now_ms;
            post_deep_sleep_diagnostic(s_deep_sleep_block_reason);
        }
    }
#endif
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

static bool ui_external_powered(void *context)
{
    (void)context;
    return external_powered();
}

static esp_err_t init_display(void)
{
    const vibe_ui_dependencies_t dependencies = {
        .brightness_changed = ui_brightness_changed,
        .external_powered = ui_external_powered,
        .context = NULL,
    };
    return vibe_ui_init(&s_ui, &dependencies);
}

static void complete_pet_fast_resume(void)
{
    s_pet_fast_resume_pending = false;
    s_woke_from_deep_sleep = false;
    vibe_ui_complete_fast_resume(&s_ui);
}

static void finish_mode_switch_visual(void)
{
    vibe_ui_finish_visual(&s_ui);
}

static void show_mode_switch_visual(const char *title,
                                    const char *hint,
                                    vibe_ui_visual_t visual,
                                    uint32_t accent_rgb)
{
    vibe_ui_show_visual(
        &s_ui, title, hint, visual, accent_rgb, false);
}

static void show_persistent_mode_switch_visual(
    const char *title,
    const char *hint,
    vibe_ui_visual_t visual,
    uint32_t accent_rgb)
{
    vibe_ui_show_visual(
        &s_ui, title, hint, visual, accent_rgb, true);
}

static void settings_touch(void)
{
    atomic_store(&s_settings_deadline_ms,
                 esp_timer_get_time() / 1000 + VIBE_STICK_SETTINGS_TIMEOUT_MS);
}

static void render_settings_visual(void)
{
    if (!settings_active()) {
        return;
    }

    char title[40] = {0};
    char hint[48] = {0};
    switch (s_settings_page) {
    case VIBE_SETTINGS_PAGE_MODE:
        snprintf(title, sizeof(title), "MODE: %s",
                 s_settings_draft_trigger == RECORDING_TRIGGER_LIFT_TO_TALK
                     ? "LIFT"
                     : "PTT");
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        strlcpy(hint, "FN VALUE  GO PAGE\nHOLD FN SAVE", sizeof(hint));
#else
        strlcpy(hint, "FRONT NEXT  HOLD SAVE", sizeof(hint));
#endif
        break;
    case VIBE_SETTINGS_PAGE_SLEEP:
        if (s_settings_draft_sleep_minutes ==
            VIBE_SETTINGS_SLEEP_DISABLED_MINUTES) {
            strlcpy(title, "SLEEP: OFF", sizeof(title));
        } else {
            snprintf(title, sizeof(title), "SLEEP: %u MIN",
                     (unsigned)s_settings_draft_sleep_minutes);
        }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        strlcpy(hint, "FN VALUE  GO PAGE\nHOLD FN SAVE", sizeof(hint));
#else
        strlcpy(hint, "FRONT NEXT  HOLD SAVE", sizeof(hint));
#endif
        break;
    case VIBE_SETTINGS_PAGE_VERSION:
        snprintf(title, sizeof(title), "FW %s", FIRMWARE_VERSION);
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        snprintf(hint, sizeof(hint), "CARD %.11s", FIRMWARE_BUILD_ID);
#else
        snprintf(hint, sizeof(hint), "STICKS3 %.11s", FIRMWARE_BUILD_ID);
#endif
        break;
    default:
        s_settings_page = VIBE_SETTINGS_PAGE_MODE;
        strlcpy(title, "MODE: PTT", sizeof(title));
        strlcpy(hint, "FRONT NEXT  HOLD SAVE", sizeof(hint));
        break;
    }

    show_persistent_mode_switch_visual(
        title, hint,
        VIBE_UI_VISUAL_DICTATION,
        current_provider_accent_rgb());
    ESP_LOGI(TAG, "settings page=%d draft_trigger=%d draft_sleep=%umin",
             (int)s_settings_page, (int)s_settings_draft_trigger,
             (unsigned)s_settings_draft_sleep_minutes);
}

static void close_settings(bool saved)
{
    atomic_store(&s_settings_active, false);
    atomic_store(&s_settings_deadline_ms, 0);
    finish_mode_switch_visual();
    render_state();
    ESP_LOGI(TAG, "settings exited saved=%d", saved ? 1 : 0);
}

static void enter_settings(void)
{
    register_activity();
    if (settings_active()) {
        settings_touch();
        return;
    }
    if (recording_network_busy() || ota_in_progress() || s_motion_calibrating ||
        atomic_load(&s_bridge_selection_active)) {
        ESP_LOGI(TAG, "settings entry ignored while device is busy");
        (void)vibe_audio_play_sound(VIBE_STICK_SOUND_ERROR);
        return;
    }

    atomic_store(&s_bridge_selection_entry_deadline_ms, 0);
    clear_ptt_followup_enter_window();
    set_motion_arm_prompt(false);
    s_settings_page = VIBE_SETTINGS_PAGE_MODE;
    s_settings_draft_trigger = s_recording_trigger_mode;
    s_settings_draft_sleep_minutes = s_sleep_minutes;
    atomic_store(&s_settings_active, true);
    settings_touch();
    render_settings_visual();
    ESP_LOGI(TAG, "settings entered trigger=%s sleep=%umin",
             recording_mode_label(), (unsigned)s_sleep_minutes);
}

static void next_settings_page(void)
{
    if (!settings_active()) {
        return;
    }
    register_activity();
    settings_touch();
    s_settings_page = vibe_settings_next_page(s_settings_page);
    render_settings_visual();
}

static void next_settings_value(void)
{
    if (!settings_active()) {
        return;
    }
    register_activity();
    settings_touch();
    if (s_settings_page == VIBE_SETTINGS_PAGE_MODE) {
        s_settings_draft_trigger =
            s_settings_draft_trigger == RECORDING_TRIGGER_PUSH_TO_TALK
                ? RECORDING_TRIGGER_LIFT_TO_TALK
                : RECORDING_TRIGGER_PUSH_TO_TALK;
    } else if (s_settings_page == VIBE_SETTINGS_PAGE_SLEEP) {
        s_settings_draft_sleep_minutes =
            vibe_settings_next_sleep_minutes(s_settings_draft_sleep_minutes);
    }
    render_settings_visual();
}

static void confirm_settings(void)
{
    if (!settings_active()) {
        return;
    }
    register_activity();
    settings_touch();

    const recording_trigger_mode_t previous_trigger = s_recording_trigger_mode;
    const uint8_t previous_sleep_minutes = s_sleep_minutes;
    esp_err_t err = ESP_OK;
    if (s_settings_draft_trigger != s_recording_trigger_mode) {
        if (s_settings_draft_trigger == RECORDING_TRIGGER_LIFT_TO_TALK) {
            err = set_lift_to_talk_trigger_mode("settings");
        } else {
            set_push_to_talk_trigger_mode();
        }
    }
    if (err == ESP_OK) {
        s_sleep_minutes = vibe_settings_sleep_minutes_sanitize(
            s_settings_draft_sleep_minutes);
        err = save_recording_mode_preference();
    }
    if (err != ESP_OK) {
        s_sleep_minutes = previous_sleep_minutes;
        if (previous_trigger == RECORDING_TRIGGER_LIFT_TO_TALK) {
            (void)set_lift_to_talk_trigger_mode("settings rollback");
        } else {
            set_push_to_talk_trigger_mode();
        }
        show_persistent_mode_switch_visual(
            "SAVE FAILED", "HOLD TO RETRY",
            VIBE_UI_VISUAL_ERROR, 0xfca5a5);
        ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
        return;
    }

    close_settings(true);
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_APPROVAL);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "settings confirmation sound skipped: %s",
                 esp_err_to_name(sound_err));
    }
    ESP_LOGI(TAG, "settings saved trigger=%s sleep=%umin",
             recording_mode_label(), (unsigned)s_sleep_minutes);
}

static void maybe_timeout_settings(int64_t now_ms)
{
    if (!settings_active()) {
        return;
    }
    const int64_t deadline_ms = atomic_load(&s_settings_deadline_ms);
    if (deadline_ms > 0 && now_ms >= deadline_ms) {
        ESP_LOGI(TAG, "settings timed out; discarding draft");
        close_settings(false);
    }
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

static void show_bridge_selection_visual(const char *status,
                                         uint32_t accent_rgb)
{
    char title[BRIDGE_TARGET_LABEL_LEN] = {0};
    bridge_selection_title(title, sizeof(title));
    show_persistent_mode_switch_visual(
        title, status,
        VIBE_UI_VISUAL_DICTATION, accent_rgb);
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
                                 target.available ? 0x86efac
                                                  : 0xfca5a5);
}

static void begin_bridge_selection_confirmation(void)
{
    char title[BRIDGE_TARGET_LABEL_LEN] = {0};
    bridge_selection_title(title, sizeof(title));
    show_persistent_mode_switch_visual(
        title, "CONFIRMING",
        VIBE_UI_VISUAL_APPROVAL, 0x93c5fd);
    s_bridge_selection_ui_phase = BRIDGE_SELECTION_UI_CONFIRMING;
    s_bridge_selection_ui_deadline_ms =
        esp_timer_get_time() / 1000 + BRIDGE_SELECTION_CONFIRM_ANIM_MS;
}

static void show_trigger_mode_switch_visual(void)
{
    const bool lift = s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK;
    show_mode_switch_visual(lift ? "LIFT TO TALK" : "PUSH TO TALK",
                            "SETTINGS MODE",
                            lift ? VIBE_UI_VISUAL_LIFT
                                 : VIBE_UI_VISUAL_PTT,
                            current_provider_accent_rgb());
}

static void show_recording_intent_switch_visual(void)
{
    if (s_recording_intent == RECORDING_INTENT_CYBER_FORTUNE) {
        show_mode_switch_visual("FORTUNE",
                                "SIDE 2X  FORT",
                                VIBE_UI_VISUAL_FORTUNE,
                                current_provider_accent_rgb());
        return;
    }
    if (s_recording_intent == RECORDING_INTENT_CYBER_ALMANAC) {
        show_mode_switch_visual("ALMANAC",
                                "SIDE 2X  ALM",
                                VIBE_UI_VISUAL_ALMANAC,
                                current_provider_accent_rgb());
        return;
    }
    show_mode_switch_visual("DICTATION",
                            "SIDE 2X  DICT",
                            VIBE_UI_VISUAL_DICTATION,
                            0xf4f5f7);
}

static void maybe_advance_bridge_selection_visual(int64_t now_ms)
{
    if (s_bridge_selection_ui_phase ==
            BRIDGE_SELECTION_UI_CONFIRMING &&
        now_ms >= s_bridge_selection_ui_deadline_ms) {
        char title[BRIDGE_TARGET_LABEL_LEN] = {0};
        bridge_selection_title(title, sizeof(title));
        show_persistent_mode_switch_visual(
            title, "CONFIRMED",
            VIBE_UI_VISUAL_DONE, 0x86efac);
        s_bridge_selection_ui_phase =
            BRIDGE_SELECTION_UI_CONFIRMED;
        s_bridge_selection_ui_deadline_ms =
            now_ms + BRIDGE_SELECTION_CONFIRMED_MS;
        ESP_LOGI(TAG, "bridge selection confirmation visible");
    } else if (s_bridge_selection_ui_phase ==
                   BRIDGE_SELECTION_UI_CONFIRMED &&
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
}

static void create_ui(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_ui_create(&s_ui));
}

static void render_state(void)
{
    if (!s_ui_ready) {
        return;
    }
    const agent_provider_config_t *provider =
        current_provider_config();
    const vibe_provider_state_t *provider_state =
        current_provider_display_state();
    bridge_target_t target;
    bridge_profile_snapshot_t profile;
    bridge_target_copy(&target);
    const bool profile_valid =
        bridge_target_profile_snapshot(&target, &profile);
    vibe_ui_view_model_t view = {
        .wifi_connected = wifi_connected(),
        .battery_valid = s_battery_display_valid,
        .battery = s_state.battery,
        .battery_charging = s_state.battery_charging,
        .usb_powered = s_state.usb_powered,
        .lift_mode =
            s_recording_trigger_mode ==
            RECORDING_TRIGGER_LIFT_TO_TALK,
        .cyber_intent = recording_intent_is_cyber(),
        .bridge_available = target.available,
        .display_active =
            s_display_power_state == DISPLAY_POWER_ACTIVE,
        .pet_fast_resume_pending = s_pet_fast_resume_pending,
        .pet_animation_resume_ms = s_pet_animation_resume_ms,
        .provider = s_state.current_provider,
    };
    strlcpy(view.wifi_ip, s_state.wifi_ip, sizeof(view.wifi_ip));
    strlcpy(
        view.mode_label, recording_mode_label(),
        sizeof(view.mode_label));
    strlcpy(
        view.intent_label, recording_intent_label(),
        sizeof(view.intent_label));
    snprintf(
        view.bridge_label, sizeof(view.bridge_label), "B %.37s",
        profile_valid && profile.label[0] != '\0'
            ? profile.label
            : "Unassigned");
    strlcpy(
        view.provider_status,
        provider->implemented ? provider_state->status
                              : "UNIMPLEMENTED",
        sizeof(view.provider_status));
    strlcpy(
        view.alert_type, s_state.alert_type,
        sizeof(view.alert_type));
    vibe_ui_render(&s_ui, &view);
}

static void show_recording_overlay(const char *title,
                                   const char *hint,
                                   bool visible)
{
    vibe_ui_show_recording(&s_ui, title, hint, visible);
    s_recording_overlay_visible = visible;
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
    const vibe_provider_state_t *display_state =
        vibe_app_state_current_provider_const(&s_state);
    ESP_LOGI(TAG, "alert type=%s project=%s message=%s",
             s_state.alert_type,
             display_state ? display_state->project : "",
             s_state.alert_message);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt->user_data) {
        return ESP_OK;
    }

    http_response_capture_t *capture = (http_response_capture_t *)evt->user_data;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        capture->connected_ms = now_ms;
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_HEADERS_SENT) {
        capture->headers_sent_ms = now_ms;
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (capture->first_response_ms == 0) {
        capture->first_response_ms = now_ms;
    }
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
    vibe_wifi_runtime_ssid(&s_wifi, ssid, ssid_len);
}

static void bridge_registry_current_ssid(char *ssid,
                                         size_t ssid_len,
                                         void *context)
{
    (void)context;
    current_wifi_ssid(ssid, ssid_len);
}

#if !defined(VIBE_BOARD_CARDPUTER_ADV)
static void current_wifi_bssid(char *bssid, size_t bssid_len)
{
    vibe_wifi_runtime_bssid(&s_wifi, bssid, bssid_len);
}
#endif

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
    vibe_bridge_registry_target(&s_bridge_registry, target);
}

static void bridge_probe_lock(void)
{
    vibe_bridge_registry_probe_lock(&s_bridge_registry);
}

static void bridge_probe_unlock(void)
{
    vibe_bridge_registry_probe_unlock(&s_bridge_registry);
}

static size_t bridge_saved_profile_count(void)
{
    return vibe_bridge_registry_saved_profile_count(&s_bridge_registry);
}

static bool bridge_saved_profile_snapshot_at(size_t index,
                                             bridge_profile_snapshot_t *snapshot)
{
    return vibe_bridge_registry_saved_profile_snapshot(
        &s_bridge_registry, index, snapshot);
}

static bool bridge_target_profile_snapshot(const bridge_target_t *target,
                                           bridge_profile_snapshot_t *snapshot)
{
    return vibe_bridge_registry_target_profile(
        &s_bridge_registry, target, snapshot);
}

#if VIBE_STICK_SERIAL_DEBUG_ENABLED
static void bridge_profiles_clear(void)
{
    vibe_bridge_registry_clear(&s_bridge_registry);
}
#endif

static int bridge_profile_index_by_id(const char *id)
{
    return vibe_bridge_registry_profile_index(&s_bridge_registry, id);
}

static bool bridge_target_set_profile(size_t profile_index, const char *source, bool available)
{
    return vibe_bridge_registry_select(
        &s_bridge_registry, profile_index, source, available);
}

static void bridge_target_note_result(const char *expected_profile_id,
                                      esp_err_t err)
{
    vibe_bridge_registry_note_result(
        &s_bridge_registry, expected_profile_id, err);
}

static esp_err_t bridge_target_save_nvs(void)
{
    return vibe_bridge_registry_save_target(&s_bridge_registry);
}

static void set_common_http_headers(esp_http_client_handle_t client, const char *token)
{
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    char id[18] = {0};
    char input_profile_revision[12] = {0};
    device_id(id, sizeof(id));
    snprintf(input_profile_revision, sizeof(input_profile_revision), "%lu",
             (unsigned long)vibe_cardputer_runtime_profile_revision(
                 &s_card_runtime));
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Name",
                               FIRMWARE_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Version",
                               FIRMWARE_VERSION);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Transport",
                               TRANSPORT);
    esp_http_client_set_header(client, "X-Vibe-Stick-Firmware-Build-Date",
                               FIRMWARE_BUILD_ID);
    esp_http_client_set_header(client, "X-Vibe-Stick-Board", VIBE_BOARD_NAME);
    esp_http_client_set_header(client, "X-Vibe-Stick-Device-Id", id);
    esp_http_client_set_header(client, "X-Vibe-Stick-Input-Profile-Revision",
                               input_profile_revision);
    if (token && token[0] != '\0') {
        esp_http_client_set_header(client, "X-Vibe-Stick-Token", token);
    }
#else
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
#endif
}

static void bridge_client_set_headers(esp_http_client_handle_t client,
                                      const char *token,
                                      void *context)
{
    (void)context;
    set_common_http_headers(client, token);
}

static esp_err_t http_request_target(const char *method, const char *host, int port,
                                     const char *token, const char *path, const char *body,
                                     char *response, int response_len, int timeout_ms)
{
    return vibe_bridge_client_request(
        &s_bridge_client, method, host, port, token, path, body,
        response, response_len, timeout_ms);
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
                   cJSON_IsString(bridge_name) &&
                   vibe_bridge_health_name_supported(bridge_name->valuestring);
    if (!healthy) {
        cJSON_Delete(root);
        return false;
    }

    memset(profile, 0, sizeof(*profile));
    strlcpy(profile->host, host, sizeof(profile->host));
    profile->port = port;
    strlcpy(profile->token, token ? token : "", sizeof(profile->token));

    bool generic_id = vibe_bridge_identity_is_generic(
        cJSON_IsString(bridge_id) ? bridge_id->valuestring : NULL);
    if (generic_id) {
        vibe_bridge_fallback_id(host, profile->id, sizeof(profile->id));
    } else {
        strlcpy(profile->id, bridge_id->valuestring, sizeof(profile->id));
    }

    bool generic_label = vibe_bridge_identity_is_generic(
        cJSON_IsString(bridge_label) ? bridge_label->valuestring : NULL);
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
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    generic_profile_id = generic_profile_id ||
                         strcmp(profile->id, CARD_SETUP_MANUAL_BRIDGE_ID) == 0;
#endif
    bool healthy = cJSON_IsBool(ok) && cJSON_IsTrue(ok) &&
                   cJSON_IsString(bridge_name) &&
                   vibe_bridge_health_name_supported(bridge_name->valuestring) &&
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

static bool bridge_profiles_merge_scan_results(const char *scan_ssid)
{
    return vibe_bridge_registry_merge_scan(
        &s_bridge_registry,
        scan_ssid,
        s_bridge_scan_profiles,
        s_bridge_scan_profile_count);
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
                                    VIBE_UI_VISUAL_DONE,
                                    0x86efac);
        }
    } else if (!atomic_load(&s_bridge_selection_active)) {
        show_mode_switch_visual("NO BRIDGE", "OFFLINE",
                                VIBE_UI_VISUAL_DICTATION,
                                0xfca5a5);
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
                VIBE_UI_VISUAL_DICTATION, 0x93c5fd);
        }
        return false;
    }
    if (show_searching && !atomic_load(&s_bridge_selection_active)) {
        show_persistent_mode_switch_visual("SEARCHING",
                                           "LAN BRIDGES",
                                           VIBE_UI_VISUAL_DICTATION,
                                           0x93c5fd);
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
    (void)vibe_bridge_registry_ensure_target(&s_bridge_registry);
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
        show_bridge_selection_visual("OFFLINE", 0xfca5a5);
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
        show_bridge_selection_visual("OFFLINE", 0xfca5a5);
        return;
    }
    (void)bridge_target_save_nvs();
    ESP_LOGI(TAG, "bridge profile selected id=%s host=%s port=%d",
             next.id, next.host, next.port);
    render_state();
    show_bridge_selection_visual("CONNECTING", 0x93c5fd);
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
    vibe_bridge_profile_snapshot_view(&profile, &profile_view);
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

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static esp_err_t card_message_request(const char *method, const char *path,
                                      const char *body, char *response,
                                      size_t response_len)
{
    if (!wifi_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    return http_request_timeout(method, path, body, response,
                                (int)response_len, 15000);
}

static esp_err_t card_message_download(const char *path,
                                       const char *destination,
                                       size_t maximum_size)
{
    static const char resource_prefix[] = "/device/messages/resource?";
    if (!path || strncmp(path, resource_prefix, strlen(resource_prefix)) != 0 ||
        !destination || maximum_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG,
                        "prepare message bridge target");
    char url[320];
    snprintf(url, sizeof(url), "http://%s:%d%s", target.host, target.port,
             path);
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 60000,
        .buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
    };
    vibe_bridge_client_lock(&s_bridge_client);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        vibe_bridge_client_unlock(&s_bridge_client);
        ESP_LOGE(TAG, "message download init");
        return ESP_ERR_NO_MEM;
    }
    bridge_profile_snapshot_t profile;
    set_common_http_headers(client,
                            bridge_target_profile_snapshot(&target, &profile)
                                ? profile.token
                                : VIBE_STICK_BRIDGE_TOKEN);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length <= 0 ||
        (uint64_t)content_length > maximum_size) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    FILE *file = fopen(destination, "wb");
    if (!file) {
        err = ESP_FAIL;
        goto cleanup;
    }
    uint8_t buffer[512];
    size_t total = 0;
    while (err == ESP_OK && total < (size_t)content_length) {
        size_t request = (size_t)content_length - total;
        if (request > sizeof(buffer)) request = sizeof(buffer);
        int count = esp_http_client_read(client, (char *)buffer, (int)request);
        if (count < 0) {
            err = ESP_FAIL;
            break;
        }
        if (count == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (fwrite(buffer, 1, (size_t)count, file) != (size_t)count) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)count;
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && total != (size_t)content_length) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) unlink(destination);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vibe_bridge_client_unlock(&s_bridge_client);
    bridge_target_note_result(target.profile_id, err);
    return err;
}
#endif

static esp_err_t http_post_binary(const char *path, const uint8_t *body, size_t body_len,
                                  char *response, int response_len)
{
    const int64_t request_start_ms = esp_timer_get_time() / 1000;
    char url[256];
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG, "prepare bridge target");
    const int64_t target_ready_ms = esp_timer_get_time() / 1000;
    snprintf(url, sizeof(url), "http://%s:%d%s", target.host, target.port, path);
    http_response_capture_t capture = {
        .data = response,
        .capacity = response_len,
        .used = 0,
    };
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = RECORDING_UPLOAD_HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
        .event_handler = http_event_handler,
        .user_data = &capture,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http init");
    const int64_t client_ready_ms = esp_timer_get_time() / 1000;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    bridge_profile_snapshot_t profile;
    set_common_http_headers(client,
                            bridge_target_profile_snapshot(&target, &profile)
                                ? profile.token
                                : VIBE_STICK_BRIDGE_TOKEN);
    esp_http_client_set_header(client, "Content-Type",
                               vibe_audio_transport_content_type());
    esp_http_client_set_header(client, "X-Vibe-Stick-Audio-Encoding",
                               vibe_audio_transport_encoding());
    esp_http_client_set_header(client, "X-Vibe-Stick-Audio-Sample-Rate",
                               "16000");
    esp_http_client_set_header(client, "X-Vibe-Stick-Audio-Channels", "1");
    if (vibe_audio_transport() == VIBE_AUDIO_TRANSPORT_IMA_ADPCM) {
        esp_http_client_set_header(client,
                                   "X-Vibe-Stick-Audio-Block-Samples", "960");
    }
    esp_http_client_set_header(client, "X-Vibe-Stick-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Vibe-Stick-Channels", "1");
    esp_http_client_set_header(client, "X-Vibe-Stick-Bits-Per-Sample", "16");
    esp_http_client_set_post_field(client, (const char *)body, body_len);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    const int64_t finished_ms = esp_timer_get_time() / 1000;
    if (err == ESP_OK && (status_code < 200 || status_code >= 300)) {
        ESP_LOGW(TAG, "http POST %s returned status=%d", path, status_code);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && response && response_len > 0 && capture.used == 0) {
        ESP_LOGW(TAG, "http POST %s status=%d empty response", path, status_code);
    }
    const int64_t connected_wait_ms =
        capture.connected_ms > 0 ? capture.connected_ms - client_ready_ms : -1;
    const int64_t request_wait_ms =
        capture.headers_sent_ms > 0
            ? capture.headers_sent_ms -
                  (capture.connected_ms > 0 ? capture.connected_ms : client_ready_ms)
            : -1;
    const int64_t response_wait_ms =
        capture.first_response_ms > 0 && capture.headers_sent_ms > 0
            ? capture.first_response_ms - capture.headers_sent_ms
            : -1;
    const int64_t target_wait_ms = target_ready_ms - request_start_ms;
    const int64_t init_wait_ms = client_ready_ms - target_ready_ms;
    recording_http_phase_stats_t *http_stats = &s_recording_http_stats;
    http_stats->samples++;
#define NOTE_HTTP_PHASE(name, value)                         \
    do {                                                     \
        const int64_t phase_ms = (value) >= 0 ? (value) : 0; \
        http_stats->name##_total_ms += phase_ms;             \
        if (phase_ms > http_stats->name##_max_ms) {          \
            http_stats->name##_max_ms = phase_ms;            \
        }                                                    \
    } while (0)
    NOTE_HTTP_PHASE(target, target_wait_ms);
    NOTE_HTTP_PHASE(init, init_wait_ms);
    NOTE_HTTP_PHASE(connect, connected_wait_ms);
    NOTE_HTTP_PHASE(headers, request_wait_ms);
    NOTE_HTTP_PHASE(response, response_wait_ms);
#undef NOTE_HTTP_PHASE
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
    ESP_LOGI(TAG,
             "recording http timing bytes=%u init_ms=%lld connect_ms=%lld "
             "send_ms=%lld response_ms=%lld total_ms=%lld status=%d",
             (unsigned)body_len,
             (long long)(client_ready_ms - request_start_ms),
             (long long)connected_wait_ms,
             (long long)request_wait_ms,
             (long long)response_wait_ms,
             (long long)(finished_ms - request_start_ms),
             status_code);
#else
    (void)request_start_ms;
    (void)client_ready_ms;
    (void)connected_wait_ms;
    (void)request_wait_ms;
    (void)response_wait_ms;
    (void)finished_ms;
#endif
    esp_http_client_cleanup(client);
    bridge_target_note_result(target.profile_id, err);
    return err;
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
typedef struct {
    bool has_carry;
    uint8_t carry;
} tts_pcm_stream_state_t;

static esp_err_t write_tts_pcm_stream(tts_pcm_stream_state_t *state,
                                      const uint8_t *data, size_t len)
{
    if (state->has_carry && len > 0) {
        const uint8_t pair[2] = {state->carry, data[0]};
        ESP_RETURN_ON_ERROR(vibe_audio_play_stream_write(pair, sizeof(pair)),
                            TAG, "write TTS carry");
        state->has_carry = false;
        data++;
        len--;
    }
    const size_t even_len = len & ~(size_t)1;
    if (even_len > 0) {
        ESP_RETURN_ON_ERROR(vibe_audio_play_stream_write(data, even_len),
                            TAG, "write TTS stream");
        data += even_len;
        len -= even_len;
    }
    if (len == 1) {
        state->carry = data[0];
        state->has_carry = true;
    }
    return ESP_OK;
}

static esp_err_t play_latest_tts_audio(void)
{
    bridge_target_t target;
    ESP_RETURN_ON_ERROR(bridge_prepare_active_target(&target), TAG,
                        "prepare bridge target");
    char url[256];
    build_bridge_url(VIBE_STICK_RECORDING_TTS_PATH, url, sizeof(url));
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
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
        bridge_target_note_result(target.profile_id, err);
        return err;
    }
    const int64_t content_length = esp_http_client_fetch_headers(client);
    const int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200 || content_length <= 0 ||
        content_length > TTS_AUDIO_MAX_BYTES) {
        ESP_LOGW(TAG, "tts stream rejected status=%d length=%lld",
                 status_code, (long long)content_length);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    const size_t buffer_capacity = 4096;
    uint8_t *buffer = heap_caps_malloc(buffer_capacity, MALLOC_CAP_8BIT);
    if (!buffer) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    size_t buffered = 0;
    size_t pcm_offset = 0;
    size_t pcm_len = 0;
    while (buffered < buffer_capacity &&
           !vibe_wav_pcm_stream_info(buffer, buffered, (size_t)content_length,
                                     VIBE_STICK_AUDIO_SAMPLE_RATE,
                                     VIBE_STICK_AUDIO_CHANNELS,
                                     VIBE_STICK_AUDIO_BITS_PER_SAMPLE,
                                     &pcm_offset, &pcm_len)) {
        int count = esp_http_client_read(client, (char *)buffer + buffered,
                                         (int)(buffer_capacity - buffered));
        if (count < 0) {
            err = ESP_FAIL;
            break;
        }
        if (count == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        buffered += (size_t)count;
    }
    if (err == ESP_OK &&
        !vibe_wav_pcm_stream_info(buffer, buffered, (size_t)content_length,
                                  VIBE_STICK_AUDIO_SAMPLE_RATE,
                                  VIBE_STICK_AUDIO_CHANNELS,
                                  VIBE_STICK_AUDIO_BITS_PER_SAMPLE,
                                  &pcm_offset, &pcm_len)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        heap_caps_free(buffer);
        goto cleanup;
    }

    ESP_LOGI(TAG, "streaming tts audio pcm_bytes=%u", (unsigned)pcm_len);
    err = vibe_audio_play_stream_begin();
    bool stream_open = err == ESP_OK;
    tts_pcm_stream_state_t stream_state = {0};
    size_t pcm_received = 0;
    if (stream_open && buffered > pcm_offset) {
        size_t available = buffered - pcm_offset;
        if (available > pcm_len) {
            available = pcm_len;
        }
        err = write_tts_pcm_stream(&stream_state, buffer + pcm_offset, available);
        pcm_received = available;
    }
    while (err == ESP_OK && pcm_received < pcm_len) {
        size_t request = pcm_len - pcm_received;
        if (request > buffer_capacity) {
            request = buffer_capacity;
        }
        int count = esp_http_client_read(client, (char *)buffer, (int)request);
        if (count < 0) {
            err = ESP_FAIL;
            break;
        }
        if (count == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        err = write_tts_pcm_stream(&stream_state, buffer, (size_t)count);
        pcm_received += (size_t)count;
    }
    if (err == ESP_OK && (pcm_received != pcm_len || stream_state.has_carry)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (stream_open) {
        esp_err_t end_err = vibe_audio_play_stream_end();
        if (err == ESP_OK) {
            err = end_err;
        }
    }
    heap_caps_free(buffer);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    bridge_target_note_result(target.profile_id, err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tts streaming failed: %s", esp_err_to_name(err));
    }
    return err;
}

#else
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
        .buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
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
    if (!vibe_wav_pcm_payload(audio, audio_len,
                              VIBE_STICK_AUDIO_SAMPLE_RATE,
                              VIBE_STICK_AUDIO_CHANNELS,
                              VIBE_STICK_AUDIO_BITS_PER_SAMPLE,
                              &pcm, &pcm_len)) {
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
#endif

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

static esp_err_t ota_request_manifest(const char *path,
                                      char *response,
                                      size_t response_len,
                                      int timeout_ms,
                                      void *context)
{
    (void)context;
    return http_request_timeout(
        "GET", path, NULL, response, response_len, timeout_ms);
}

static void ota_build_url(const char *path_or_url,
                          char *url,
                          size_t url_len,
                          void *context)
{
    (void)context;
    build_bridge_url(path_or_url, url, url_len);
}

static esp_err_t ota_prepare_download(void *context)
{
    (void)context;
    bridge_target_t target;
    return bridge_prepare_active_target(&target);
}

static esp_err_t ota_prepare_client(esp_http_client_handle_t client,
                                    void *context)
{
    (void)context;
    bridge_target_t target;
    bridge_target_copy(&target);
    bridge_profile_snapshot_t profile;
    set_common_http_headers(
        client,
        bridge_target_profile_snapshot(&target, &profile)
            ? profile.token
            : VIBE_STICK_BRIDGE_TOKEN);
    return ESP_OK;
}

static bool ota_online(void *context)
{
    (void)context;
    return wifi_connected();
}

static bool ota_busy(void *context)
{
    (void)context;
    return recording_network_busy();
}

static bool ota_external_powered(void *context)
{
    (void)context;
    return external_powered();
}

static bool ota_display_active(void *context)
{
    (void)context;
    return s_display_power_state == DISPLAY_POWER_ACTIVE;
}

static bool ota_settings_active(void *context)
{
    (void)context;
    return settings_active();
}

static void ota_overlay(const char *title,
                        const char *hint,
                        bool visible,
                        void *context)
{
    (void)context;
    show_recording_overlay(title, hint, visible);
}

static void ota_before_start(void *context)
{
    (void)context;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    vibe_cardputer_runtime_stop_interactive(&s_card_runtime);
#endif
}

static void start_ota_check_task(void)
{
    (void)vibe_ota_runtime_handle(
        &s_ota, VIBE_OTA_COMMAND_CHECK);
}

static bool parse_state_json(const char *json)
{
    vibe_state_update_t update;
    if (!vibe_state_json_apply(&s_state, json, &update)) {
        return false;
    }
    if (update.tts_requested) {
        if (!recording_network_busy()) {
            (void)queue_event(VIBE_STICK_EVENT_TTS_PROBE);
        } else {
            ESP_LOGI(TAG, "tts probe deferred while recording network is busy");
        }
    }
    return true;
}

static void update_battery_display_level(int raw_level, int64_t now_ms)
{
    const vibe_power_runtime_command_t command = {
        .type = VIBE_POWER_COMMAND_RECORD_BATTERY,
        .data.battery = {
            .raw_level = raw_level,
            .now_ms = now_ms,
            .woke_from_deep_sleep = s_woke_from_deep_sleep,
            .retained_valid =
                s_retained_battery_magic ==
                    VIBE_STICK_BATTERY_RTC_MAGIC,
            .retained_level = s_retained_battery_display_level,
        },
    };
    (void)vibe_power_runtime_handle(&s_power, &command);
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
        const vibe_power_runtime_command_t command = {
            .type = VIBE_POWER_COMMAND_SET_VOLTAGE,
            .data.value = battery_voltage_mv,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
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
        const vibe_power_runtime_command_t command = {
            .type = VIBE_POWER_COMMAND_SET_CHARGING,
            .data.flag = charging,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
        power_read_ok = true;
    }
    if (vibe_board_usb_powered(&usb_powered) == ESP_OK) {
        s_state.usb_powered = usb_powered;
        const vibe_power_runtime_command_t command = {
            .type = VIBE_POWER_COMMAND_SET_USB_POWERED,
            .data.flag = usb_powered,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
        power_read_ok = true;
    }
    if (power_read_ok && was_external_powered && !external_powered()) {
        ESP_LOGI(TAG, "external power removed");
        const vibe_power_runtime_command_t command = {
            .type =
                VIBE_POWER_COMMAND_SET_EXTERNAL_POWER_REMOVED_TIME,
            .data.time_ms = now_ms,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
    }
#if VIBE_BOARD_HOLD_FULL_BATTERY_ICON
    if (!external_powered()) {
        const vibe_power_runtime_command_t command = {
            .type = VIBE_POWER_COMMAND_SET_FULL_LATCH,
            .data.flag = false,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
    } else if (battery_read_ok &&
               battery_level >= VIBE_STICK_BATTERY_FULL_LATCH_PERCENT) {
        const vibe_power_runtime_command_t command = {
            .type = VIBE_POWER_COMMAND_SET_FULL_LATCH,
            .data.flag = true,
        };
        (void)vibe_power_runtime_handle(&s_power, &command);
    }
#endif
#if CONFIG_PM_ENABLE && VIBE_BOARD_HAS_GPIO_BACKLIGHT
    if (power_read_ok) {
        update_display_light_sleep_lock(
            !vibe_ui_rendering_suspended(&s_ui));
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
        vibe_provider_state_t *display_state = current_provider_display_state();
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

static void post_deep_sleep_diagnostic(const char *reason)
{
    char body[160];
    snprintf(body, sizeof(body),
             "{\"event\":\"deep_sleep_blocked\",\"source\":\"%s\","
             "\"status\":\"%s\"}",
             VIBE_BOARD_EVENT_SOURCE, reason ? reason : "unknown");
    char response[512] = {0};
    (void)http_request_timeout("POST", VIBE_STICK_EVENT_PATH, body, response,
                               sizeof(response), 1200);
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
    vibe_recording_followup_clear(&s_ptt_followup);
}

static void arm_ptt_followup_enter_window(void)
{
    (void)vibe_recording_followup_arm(&s_ptt_followup,
                                      s_recording_session_id,
                                      esp_timer_get_time() / 1000,
                                      PTT_ENTER_GRACE_MS);
}

static bool consume_ptt_followup_enter_window(void)
{
    return vibe_recording_followup_consume(
        &s_ptt_followup,
        s_recording_trigger_mode == RECORDING_TRIGGER_PUSH_TO_TALK,
        esp_timer_get_time() / 1000);
}

static bool ptt_followup_enter_window_present(void)
{
    return vibe_recording_followup_present(&s_ptt_followup);
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
    if (!event_name || s_ptt_followup.session_id[0] == '\0' ||
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
    strlcpy(dispatch->session_id, s_ptt_followup.session_id, sizeof(dispatch->session_id));
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

static bool parse_recording_capture_mode(const char *json, char *capture_mode,
                                         size_t capture_mode_len)
{
    if (!capture_mode || capture_mode_len == 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *mode = cJSON_IsObject(recording)
                      ? cJSON_GetObjectItemCaseSensitive(recording, "capture_mode")
                      : NULL;
    bool ok = cJSON_IsString(mode) && mode->valuestring &&
              mode->valuestring[0] != '\0';
    if (ok) {
        strlcpy(capture_mode, mode->valuestring, capture_mode_len);
    }
    cJSON_Delete(root);
    return ok;
}

static bool parse_recording_transport_encoding(const char *json,
                                               char *encoding,
                                               size_t encoding_len)
{
    if (!encoding || encoding_len == 0) {
        return false;
    }
    encoding[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *recording = cJSON_GetObjectItemCaseSensitive(root, "recording");
    cJSON *item =
        cJSON_IsObject(recording)
            ? cJSON_GetObjectItemCaseSensitive(
                  recording, "accepted_transport_encoding")
            : NULL;
    const bool ok = cJSON_IsString(item) && item->valuestring &&
                    item->valuestring[0] != '\0';
    if (ok) {
        strlcpy(encoding, item->valuestring, encoding_len);
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
           strcmp(status, "stop_failed") == 0 ||
           strcmp(status, "recording_failed") == 0;
}

static uint32_t recording_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < len; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return crc ^ UINT32_MAX;
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
    return vibe_wifi_runtime_rssi(&s_wifi);
}

static bool recording_begin_session(const char *session_id,
                                    bool notify_bridge)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_BEGIN_SESSION,
        .data.begin_session = {
            .session_id = session_id,
            .notify_bridge = notify_bridge,
        },
    };
    const bool started =
        vibe_recording_controller_handle(&s_recording, &command);
    if (started) {
        update_status_led();
    }
    return started;
}

static void recording_reset_session(void)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_RESET_SESSION,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
    update_status_led();
}

static void recording_set_capture_mode(bool local_capture)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_CAPTURE_MODE,
        .data.flag = local_capture,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
}

static void recording_set_tap_active(bool active)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_TAP_ACTIVE,
        .data.flag = active,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
}

static void recording_set_motion_active(bool active)
{
    const vibe_recording_command_t command = {
        .type = VIBE_RECORDING_COMMAND_SET_MOTION_ACTIVE,
        .data.flag = active,
    };
    (void)vibe_recording_controller_handle(&s_recording, &command);
}

static esp_err_t upload_recording_chunk(const uint8_t *audio, size_t audio_len,
                                        uint32_t chunk_id, void *context)
{
    (void)context;
    if (!audio || audio_len == 0 || s_recording_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    uint32_t chunk_crc32 = recording_crc32(audio, audio_len);
    snprintf(path, sizeof(path), "%s?session_id=%s&chunk_id=%lu&chunk_crc32=%08lx",
             VIBE_STICK_RECORDING_AUDIO_PATH, s_recording_session_id,
             (unsigned long)chunk_id,
             (unsigned long)chunk_crc32);
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

static bool start_recording_upload_task(void)
{
    memset(&s_recording_http_stats, 0, sizeof(s_recording_http_stats));
    const vibe_recording_upload_config_t config = {
        .buffer_bytes = RECORDING_UPLOAD_BUFFER_BYTES,
        .batch_chunks = RECORDING_UPLOAD_BATCH_CHUNKS,
        .parallel_uploads = RECORDING_UPLOAD_PARALLEL_WORKERS,
        .read_timeout_ms = 250,
        .task_stack_bytes = 5120,
        .task_priority = 4,
        .task_core = VIBE_STICK_NETWORK_CORE,
        .post_chunk = upload_recording_chunk,
        .context = NULL,
    };
    return vibe_recording_upload_start(
        &config, current_wifi_rssi(), RECORDING_RSSI_UNKNOWN);
}

static void notify_bridge_recording_start_failed(const char *event_name,
                                                 const char *reason)
{
    if (!s_recording_bridge_stop_required ||
        s_recording_session_id[0] == '\0') {
        return;
    }
    char body[512];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"paste\":false,"
             "\"session_id\":\"%s\",\"intent\":\"%s\",\"mode\":\"%s\","
             "\"protocol_version\":2,\"total_chunks\":0,\"total_bytes\":0,"
             "\"total_wire_bytes\":0,\"transport_encoding\":\"%s\","
             "\"dropped_chunks\":0,\"dropped_bytes\":0,"
             "\"upload_failed\":true,\"error\":\"%s\"}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             s_recording_session_id,
             recording_mode_intent(),
             recording_mode_label(),
             vibe_audio_transport_encoding(),
             reason);
    char response[256] = {0};
    esp_err_t err = http_request_timeout(
        "POST", VIBE_STICK_RECORDING_STOP_PATH, body, response,
        sizeof(response), RECORDING_START_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recording start failure cleanup failed: %s",
                 esp_err_to_name(err));
    }
}

static bool handle_recording_start_internal(const char *event_name, const char *hint,
                                            const char *provided_session_id,
                                            bool notify_bridge)
{
    ESP_LOGI(TAG, "recording start heap_free=%u heap_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    vibe_cardputer_runtime_release_display_resources(&s_card_runtime);
    if (vibe_cardputer_runtime_messages_busy(&s_card_runtime)) {
        ESP_LOGI(TAG, "recording start ignored while message center is busy");
        return false;
    }
#endif
    register_activity();
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    vibe_cardputer_runtime_stop_interactive(&s_card_runtime);
#endif
    clear_ptt_followup_enter_window();
    clear_cyber_tts_wait();
    if (recording_network_busy() || s_tap_recording_active || s_motion_recording_active) {
        ESP_LOGI(TAG, "recording start ignored while already recording");
        return false;
    }
    char session_id[VIBE_RECORDING_POLICY_SESSION_ID_LEN] = {0};
    if (provided_session_id && provided_session_id[0] != '\0') {
        strlcpy(session_id, provided_session_id, sizeof(session_id));
    } else {
        generate_recording_session_id(session_id, sizeof(session_id));
    }
    if (!recording_begin_session(session_id, notify_bridge)) {
        ESP_LOGW(TAG, "recording start failed: no session id");
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_wifi_runtime_set_performance(&s_wifi, true));
    show_recording_overlay("CONNECTING", "", true);
    char negotiated_transport[24] = {0};
    const char *offered_transport = "pcm16";

    if (notify_bridge) {
        esp_err_t transport_err =
            vibe_audio_set_transport(VIBE_AUDIO_TRANSPORT_IMA_ADPCM);
        if (transport_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "ADPCM preflight failed, offering PCM instead: %s",
                     esp_err_to_name(transport_err));
            transport_err =
                vibe_audio_set_transport(VIBE_AUDIO_TRANSPORT_PCM16);
        }
        if (transport_err != ESP_OK) {
            ESP_LOGW(TAG, "audio transport preflight failed: %s",
                     esp_err_to_name(transport_err));
            show_recording_overlay("MIC FAILED", "", true);
            recording_reset_session();
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                vibe_wifi_runtime_set_performance(&s_wifi, false));
            return false;
        }
        offered_transport = vibe_audio_transport_encoding();
    }

    ESP_LOGI(TAG, "recording start event=%s mode=%s intent=%s session=%s",
             event_name,
             recording_mode_label(),
             recording_mode_intent(),
             s_recording_session_id);
    if (notify_bridge) {
        char body[384];
        snprintf(body, sizeof(body),
                 "{\"event\":\"%s\",\"source\":\"%s\","
                 "\"audio_source\":\"%s\",\"session_id\":\"%s\","
                 "\"intent\":\"%s\",\"mode\":\"%s\",\"protocol_version\":2,"
                 "\"transport_encoding\":\"%s\"}",
                 event_name,
                 VIBE_BOARD_EVENT_SOURCE,
                 VIBE_BOARD_AUDIO_SOURCE,
                 s_recording_session_id,
                 recording_mode_intent(),
                 recording_mode_label(),
                 offered_transport);
        char response[1024] = {0};
        esp_err_t err = http_request_timeout(
            "POST", VIBE_STICK_RECORDING_START_PATH, body, response,
            sizeof(response), RECORDING_START_TIMEOUT_MS);
        if (err == ESP_OK && response[0] != '\0') {
            char response_session_id[40] = {0};
            char capture_mode[24] = "device_upload";
            parse_recording_session_id(response, response_session_id,
                                       sizeof(response_session_id));
            parse_recording_capture_mode(response, capture_mode,
                                         sizeof(capture_mode));
            parse_recording_transport_encoding(
                response, negotiated_transport, sizeof(negotiated_transport));
            if (response_session_id[0] != '\0' &&
                strcmp(response_session_id, s_recording_session_id) != 0) {
                ESP_LOGW(TAG, "bridge returned a different recording session id");
                const vibe_recording_command_t command = {
                    .type = VIBE_RECORDING_COMMAND_SET_SESSION_ID,
                    .data.session_id = response_session_id,
                };
                (void)vibe_recording_controller_handle(
                    &s_recording, &command);
            }
            recording_set_capture_mode(
                strcmp(capture_mode, "device_upload") == 0);
            ESP_LOGI(TAG, "recording capture mode=%s local=%d",
                     capture_mode, s_recording_local_capture ? 1 : 0);
            if (parse_state_json(response)) {
                complete_pet_fast_resume();
                render_state();
            }
        } else {
            ESP_LOGW(TAG, "recording start bridge request failed: %s",
                     esp_err_to_name(err));
            show_recording_overlay("CONNECT FAILED", "", true);
            vTaskDelay(pdMS_TO_TICKS(700));
            show_recording_overlay(NULL, NULL, false);
            recording_reset_session();
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                vibe_wifi_runtime_set_performance(&s_wifi, false));
            return false;
        }
    }

    if (s_recording_local_capture) {
        vibe_audio_transport_t transport = VIBE_AUDIO_TRANSPORT_PCM16;
        if (strcmp(negotiated_transport,
                   VIBE_STICK_AUDIO_ADPCM_ENCODING) == 0) {
            transport = VIBE_AUDIO_TRANSPORT_IMA_ADPCM;
        }
        esp_err_t transport_err = vibe_audio_set_transport(transport);
        if (transport_err != ESP_OK) {
            ESP_LOGW(TAG, "audio transport setup failed: %s",
                     esp_err_to_name(transport_err));
            notify_bridge_recording_start_failed(
                "device_capture_start_failed", "audio_transport_setup");
            show_recording_overlay("MIC FAILED", "", true);
            recording_reset_session();
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                vibe_wifi_runtime_set_performance(&s_wifi, false));
            return false;
        }
    }

    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_RECORDING_START);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording start sound skipped: %s", esp_err_to_name(sound_err));
    }

    if (!s_recording_local_capture) {
        show_recording_overlay("REMOTE MIC", hint, true);
        return true;
    }

    esp_err_t audio_err = vibe_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "hardware recording start failed: %s", esp_err_to_name(audio_err));
        notify_bridge_recording_start_failed(
            "device_capture_start_failed", "hardware_recording_start");
        show_recording_overlay("MIC FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        recording_reset_session();
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return false;
    }
    if (!start_recording_upload_task()) {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        notify_bridge_recording_start_failed(
            "device_capture_start_failed", "upload_task_start");
        show_recording_overlay("SEND FAILED", "", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
        recording_reset_session();
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return false;
    }
    show_recording_overlay("LISTENING", hint, true);
    return true;
}

static bool handle_recording_start(const char *event_name, const char *hint)
{
    return handle_recording_start_internal(event_name, hint, NULL, true);
}

static void finish_recording_stop(const char *event_name)
{
    bool upload_failed = vibe_recording_upload_failed();
    vibe_audio_stats_t final_audio_stats = {0};
    vibe_recording_upload_diagnostics_t upload_diagnostics = {0};
    if (s_recording_local_capture) {
        esp_err_t audio_err = vibe_audio_stop();
        if (audio_err != ESP_OK) {
            ESP_LOGW(TAG, "hardware recording stop failed: %s",
                     esp_err_to_name(audio_err));
        }
        vibe_recording_upload_wait();
        upload_failed = vibe_recording_upload_failed();
        vibe_audio_stats(&final_audio_stats);
        vibe_recording_upload_diagnostics(&upload_diagnostics);
        if (final_audio_stats.chunks_dropped > 0) {
            upload_failed = true;
            ESP_LOGW(TAG,
                     "recording marked failed after capture drops chunks=%u "
                     "bytes=%u",
                     (unsigned)final_audio_stats.chunks_dropped,
                     (unsigned)final_audio_stats.bytes_dropped);
        }
        size_t uploaded_posts = 0;
        size_t uploaded_bytes = 0;
        vibe_recording_upload_totals(&uploaded_posts, &uploaded_bytes);
        const vibe_recording_command_t totals_command = {
            .type = VIBE_RECORDING_COMMAND_SET_UPLOAD_TOTALS,
            .data.upload_totals = {
                .chunk_count = (uint32_t)uploaded_posts,
                .uploaded_bytes = (uint32_t)uploaded_bytes,
            },
        };
        (void)vibe_recording_controller_handle(
            &s_recording, &totals_command);
        vibe_recording_upload_log_diagnostics(VIBE_BOARD_NAME,
                                              current_wifi_rssi());
        persist_recording_diagnostic();
        vibe_audio_clear();
    }
    esp_err_t sound_err = vibe_audio_play_sound(VIBE_STICK_SOUND_RECORDING_STOP);
    if (sound_err != ESP_OK) {
        ESP_LOGW(TAG, "recording stop sound skipped: %s", esp_err_to_name(sound_err));
    }

    if (!s_recording_bridge_stop_required) {
        ESP_LOGI(TAG, "remote recording complete session=%s",
                 s_recording_session_id);
        recording_reset_session();
        show_recording_overlay(NULL, NULL, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
        return;
    }

    show_recording_overlay("TRANSCRIBING", "", true);
    bool paste_result = !recording_intent_is_cyber();
    ESP_LOGI(TAG, "recording stop event=%s mode=%s intent=%s session=%s paste=%d",
             event_name,
             recording_mode_label(),
             recording_mode_intent(),
             s_recording_session_id,
             paste_result ? 1 : 0);
    char body[512];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"source\":\"%s\",\"paste\":%s,"
             "\"session_id\":\"%s\",\"intent\":\"%s\",\"mode\":\"%s\","
             "\"protocol_version\":2,\"total_chunks\":%lu,\"total_bytes\":%lu,"
             "\"total_wire_bytes\":%lu,\"transport_encoding\":\"%s\","
             "\"dropped_chunks\":%lu,\"dropped_bytes\":%lu,"
             "\"upload_failed\":%s}",
             event_name,
             VIBE_BOARD_EVENT_SOURCE,
             paste_result ? "true" : "false",
             s_recording_session_id,
             recording_mode_intent(),
             recording_mode_label(),
             (unsigned long)s_recording_chunk_id,
             (unsigned long)s_recording_uploaded_bytes,
             (unsigned long)upload_diagnostics.upload.uploaded_wire_bytes,
             vibe_audio_transport_encoding(),
             (unsigned long)final_audio_stats.chunks_dropped,
             (unsigned long)final_audio_stats.bytes_dropped,
             upload_failed ? "true" : "false");
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
    if (err != ESP_OK || recording_failed || upload_failed) {
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
    recording_reset_session();
    poll_state();
    if (!s_cyber_tts_waiting) {
        show_recording_overlay(NULL, NULL, false);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        vibe_wifi_runtime_set_performance(&s_wifi, false));
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
    recording_set_tap_active(false);

    if (s_recording_session_id[0] == '\0') {
        (void)vibe_audio_stop();
        vibe_audio_clear();
        clear_ptt_followup_enter_window();
        poll_state();
        show_recording_overlay(NULL, NULL, false);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
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
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    /* Cardputer-Adv has no PSRAM. Release the capture and upload task stacks
     * before allocating the unchanged main-branch finalize task. */
    if (s_recording_local_capture) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_audio_stop());
        vibe_recording_upload_wait();
    }
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(recording_finalize_task, "recording_finalize", 8192,
                                            NULL, 4, &s_recording_finalize_task,
                                            VIBE_STICK_NETWORK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "recording finalize task create failed; session aborted safely");
        set_recording_finalize_active(false);
        s_recording_finalize_task = NULL;
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        recording_reset_session();
        show_recording_overlay("SEND FAILED", "", true);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            vibe_wifi_runtime_set_performance(&s_wifi, false));
#else
        finish_recording_stop(event_name);
#endif
    }
}

static void post_device_command_ack(const char *command_id, const char *status,
                                    const char *session_id, const char *error)
{
    char body[320];
    snprintf(body, sizeof(body),
             "{\"command_id\":\"%s\",\"status\":\"%s\","
             "\"session_id\":\"%s\",\"error\":\"%s\"}",
             command_id ? command_id : "",
             status ? status : "failed",
             session_id ? session_id : "",
             error ? error : "");
    char response[256] = {0};
    esp_err_t err = http_request(
        "POST", VIBE_STICK_DEVICE_COMMAND_ACK_PATH, body,
        response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device command ack failed id=%s: %s",
                 command_id ? command_id : "-", esp_err_to_name(err));
    }
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static bool json_number_in_range(cJSON *object, const char *key, float min,
                                 float max, float *target)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < min ||
        item->valuedouble > max) {
        return false;
    }
    *target = (float)item->valuedouble;
    return true;
}

static bool json_bool_value(cJSON *object, const char *key, bool *target)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsBool(item)) return false;
    *target = cJSON_IsTrue(item);
    return true;
}

static esp_err_t apply_card_input_profile_payload(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) return ESP_ERR_INVALID_ARG;
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(payload,
                                                       "profile_revision");
    cJSON *routes = cJSON_GetObjectItemCaseSensitive(payload, "opt_routes");
    cJSON *mouse = cJSON_GetObjectItemCaseSensitive(payload, "air_mouse");
    if (!cJSON_IsNumber(revision) || revision->valuedouble < 1 ||
        !cJSON_IsObject(routes) || !cJSON_IsObject(mouse)) {
        return ESP_ERR_INVALID_ARG;
    }

    vibe_cardputer_runtime_snapshot_t snapshot;
    vibe_cardputer_runtime_snapshot(&s_card_runtime, &snapshot);
    vibe_card_input_profile_t next = snapshot.input_profile;
    next.revision = (uint32_t)revision->valuedouble;
    cJSON *tap = cJSON_GetObjectItemCaseSensitive(routes, "opt_tap");
    cJSON *double_click = cJSON_GetObjectItemCaseSensitive(routes,
                                                           "opt_double");
    cJSON *hold = cJSON_GetObjectItemCaseSensitive(routes, "opt_hold");
    if (!cJSON_IsString(tap) || !cJSON_IsString(double_click) ||
        !cJSON_IsString(hold) ||
        !vibe_card_input_route_parse(tap->valuestring, &next.opt_tap) ||
        !vibe_card_input_route_parse(double_click->valuestring,
                                     &next.opt_double) ||
        !vibe_card_input_route_parse(hold->valuestring, &next.opt_hold) ||
        !json_bool_value(mouse, "invert_horizontal",
                         &next.air_mouse.invert_horizontal) ||
        !json_bool_value(mouse, "invert_vertical",
                         &next.air_mouse.invert_vertical) ||
        !json_bool_value(mouse, "invert_scroll",
                         &next.air_mouse.invert_scroll) ||
        !json_number_in_range(mouse, "pointer_speed", 0.5f, 2.5f,
                              &next.air_mouse.pointer_speed) ||
        !json_number_in_range(mouse, "wheel_speed", 0.5f, 2.0f,
                              &next.air_mouse.wheel_speed) ||
        !json_number_in_range(mouse, "pointer_deadzone_dps", 1.0f, 6.0f,
                              &next.air_mouse.pointer_deadzone_dps) ||
        !json_number_in_range(mouse, "wheel_deadzone_dps", 2.0f, 10.0f,
                              &next.air_mouse.wheel_deadzone_dps) ||
        !vibe_card_input_profile_valid(&next)) {
        return ESP_ERR_INVALID_ARG;
    }
    return vibe_cardputer_runtime_apply_profile(
        &s_card_runtime, &next);
}
#endif

static void process_device_command(const char *response)
{
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        return;
    }
    cJSON *cursor = cJSON_GetObjectItemCaseSensitive(root, "cursor");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsNumber(cursor) && cursor->valuedouble >= 0) {
        s_device_command_cursor = (uint32_t)cursor->valuedouble;
    }
    if (!cJSON_IsObject(command)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *command_cursor =
        cJSON_GetObjectItemCaseSensitive(command, "cursor");
    cJSON *command_id =
        cJSON_GetObjectItemCaseSensitive(command, "command_id");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(command, "type");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    cJSON *session_id = cJSON_IsObject(payload)
                            ? cJSON_GetObjectItemCaseSensitive(
                                  payload, "session_id")
                            : NULL;
    if (cJSON_IsNumber(command_cursor) && command_cursor->valuedouble >= 0) {
        s_device_command_cursor = (uint32_t)command_cursor->valuedouble;
    }
    if (!cJSON_IsString(command_id) || !command_id->valuestring ||
        !cJSON_IsString(type) || !type->valuestring ||
        !cJSON_IsString(session_id) || !session_id->valuestring) {
        cJSON_Delete(root);
        return;
    }

    char command_id_text[48];
    char type_text[32];
    char session_id_text[40];
    strlcpy(command_id_text, command_id->valuestring,
            sizeof(command_id_text));
    strlcpy(type_text, type->valuestring, sizeof(type_text));
    strlcpy(session_id_text, session_id->valuestring,
            sizeof(session_id_text));
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    if (strcmp(type_text, "input_profile_update") == 0) {
        esp_err_t profile_err = apply_card_input_profile_payload(payload);
        cJSON_Delete(root);
        post_device_command_ack(command_id_text,
                                profile_err == ESP_OK ? "completed" : "failed",
                                session_id_text,
                                profile_err == ESP_OK ? "" :
                                esp_err_to_name(profile_err));
        return;
    }
#endif
    cJSON_Delete(root);

    if (strcmp(type_text, "recording_start") == 0) {
        bool started = handle_recording_start_internal(
            "remote_command_start", "REMOTE", session_id_text, false);
        post_device_command_ack(command_id_text,
                                started ? "started" : "failed",
                                session_id_text,
                                started ? "" : "recording start failed");
        return;
    }
    if (strcmp(type_text, "recording_stop") == 0) {
        if (strcmp(s_recording_session_id, session_id_text) != 0 ||
            s_recording_session_id[0] == '\0') {
            post_device_command_ack(command_id_text, "failed",
                                    session_id_text,
                                    "recording session not active");
            return;
        }
        show_recording_overlay("SENDING", "", true);
        recording_set_tap_active(false);
        finish_recording_stop("remote_command_stop");
        post_device_command_ack(command_id_text, "completed",
                                session_id_text, "");
        return;
    }
    post_device_command_ack(command_id_text, "ignored",
                            session_id_text, "unknown command");
}

static void device_command_task(void *arg)
{
    (void)arg;
    const int poll_timeout_ms =
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        0;
#else
        DEVICE_COMMAND_POLL_TIMEOUT_MS;
#endif
    while (true) {
        if (!wifi_connected() || ota_in_progress() ||
            recording_network_busy()) {
            vTaskDelay(pdMS_TO_TICKS(DEVICE_COMMAND_RETRY_DELAY_MS));
            continue;
        }
        char path[128];
        snprintf(path, sizeof(path), "%s?cursor=%lu&timeout_ms=%d",
                 VIBE_STICK_DEVICE_COMMAND_POLL_PATH,
                 (unsigned long)s_device_command_cursor,
                 poll_timeout_ms);
        char response[1024] = {0};
        esp_err_t err = http_request_timeout(
            "GET", path, NULL, response, sizeof(response),
            poll_timeout_ms + 5000);
        if (err == ESP_OK && response[0] != '\0') {
            process_device_command(response);
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "device command poll failed: %s",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(DEVICE_COMMAND_RETRY_DELAY_MS));
        }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        vTaskDelay(pdMS_TO_TICKS(CARDPUTER_COMMAND_POLL_INTERVAL_MS));
#endif
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
    if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
        if (s_motion_recording_active) {
            ESP_LOGI(TAG, "front tap stopping active LIFT recording");
            recording_set_motion_active(false);
            s_motion_lift_armed = false;
            s_motion_start_pending = false;
            s_motion_wake_confirm_pending = false;
            s_motion_wake_confirm_deadline_ms = 0;
            s_motion_wake_network_pending = false;
            s_motion_wake_network_deadline_ms = 0;
            set_motion_arm_prompt(false);
            handle_recording_stop("motion_button_stop");
        } else if (s_motion_start_pending || s_motion_wake_confirm_pending ||
                   s_motion_wake_network_pending) {
            ESP_LOGI(TAG, "front tap cancelling pending LIFT recording");
            s_motion_lift_armed = false;
            s_motion_start_pending = false;
            s_motion_wake_confirm_pending = false;
            s_motion_wake_confirm_deadline_ms = 0;
            s_motion_wake_network_pending = false;
            s_motion_wake_network_deadline_ms = 0;
            show_recording_overlay(NULL, NULL, false);
        } else {
            ESP_LOGI(TAG, "front tap ignored while LIFT is waiting for motion");
        }
        return;
    }
    if (s_tap_recording_active || vibe_audio_is_recording() || s_recording_session_id[0] != '\0') {
        handle_recording_stop("button_tap_stop");
        recording_set_tap_active(false);
        return;
    }
    recording_set_tap_active(
        handle_recording_start("button_tap_start", "TAP TO SEND"));
}

static void request_wifi_reconnect_now(void)
{
    vibe_wifi_runtime_reconnect_now(&s_wifi);
}

static bool wifi_deep_sleep_committed(void *context)
{
    (void)context;
    return atomic_load(&s_deep_sleep_committed);
}

static void wifi_status_changed(bool connected, const char *ip, void *context)
{
    (void)context;
    s_state.wifi = connected;
    strlcpy(s_state.wifi_ip, ip ? ip : "", sizeof(s_state.wifi_ip));
    render_state();
    if (connected) {
        queue_event(VIBE_STICK_EVENT_POLL_STATE);
        queue_event(VIBE_STICK_EVENT_OTA_CHECK);
    }
}

static esp_err_t init_wifi(void)
{
    const vibe_wifi_runtime_config_t config = {
        .configured_profiles = k_configured_wifi_profiles,
        .configured_profile_count =
            sizeof(k_configured_wifi_profiles) /
            sizeof(k_configured_wifi_profiles[0]),
        .idle_power_save = VIBE_STICK_WIFI_IDLE_PS,
        .max_tx_power = VIBE_BOARD_WIFI_MAX_TX_POWER,
        .deep_sleep_committed = wifi_deep_sleep_committed,
        .status_changed = wifi_status_changed,
    };
    return vibe_wifi_runtime_init(&s_wifi, &config);
}

#if VIBE_STICK_ANIM_PREVIEW
static void request_anim_preview_switch(const char *source)
{
    ESP_LOGI(TAG, "anim preview switch requested source=%s", source);
    vibe_ui_request_preview_switch(&s_ui);
}
#endif

static void button_press_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    s_front_button_iot_down_ms = now_ms;
    s_front_status_led_pressed = true;
    update_status_led();
    if (settings_active()) {
        settings_touch();
        ESP_LOGI(TAG, "front button reserved for settings");
        return;
    }
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
    if (settings_active()) {
        settings_touch();
        queue_event(VIBE_STICK_EVENT_SETTINGS_VALUE_NEXT);
        return;
    }
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
    queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_SINGLE);
}

static void button_double_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (settings_active()) {
        settings_touch();
        ESP_LOGI(TAG, "front double click ignored in settings");
        return;
    }
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
    queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_DOUBLE);
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
    if (settings_active()) {
        s_side_button_calibration_hold_reached = false;
        s_side_button_mode_hold_reached = false;
        settings_touch();
        return;
    }
    if (s_side_button_calibration_hold_reached) {
        s_side_button_calibration_hold_reached = false;
        s_side_button_mode_hold_reached = false;
        if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
            queue_input_signal(VIBE_INPUT_SIGNAL_SIDE_CALIBRATE);
        } else {
            queue_input_signal(VIBE_INPUT_SIGNAL_SIDE_MODE);
        }
        return;
    }
    if (s_side_button_mode_hold_reached) {
        s_side_button_mode_hold_reached = false;
        queue_event(VIBE_STICK_EVENT_SETTINGS_ENTER);
        return;
    }
}

static void side_button_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (settings_active()) {
        settings_touch();
        queue_event(VIBE_STICK_EVENT_SETTINGS_PAGE_NEXT);
        return;
    }
    ESP_LOGI(TAG, "side button single click ignored; double click to scan");
}

static void side_button_double_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (settings_active()) {
        settings_touch();
        ESP_LOGI(TAG, "side double click ignored in settings");
        return;
    }
    const bool can_arm = !recording_network_busy();
    atomic_store(&s_bridge_selection_entry_deadline_ms,
                 can_arm ? esp_timer_get_time() / 1000 +
                               BRIDGE_SELECTION_ENTRY_WINDOW_MS
                         : 0);
    ESP_LOGI(TAG, "side button double click: full bridge scan entry_window=%d",
             can_arm ? 1 : 0);
    queue_input_signal(VIBE_INPUT_SIGNAL_SIDE_SCAN);
}

static void button_long_start_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (settings_active()) {
        settings_touch();
        ESP_LOGI(TAG, "front PTT hold suppressed in settings");
        return;
    }
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
    queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_LONG_START);
}

static void bridge_selection_confirm_long_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    register_activity();
    if (settings_active()) {
        settings_touch();
        queue_event(VIBE_STICK_EVENT_SETTINGS_CONFIRM);
        return;
    }
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
    s_front_status_led_pressed = false;
    update_status_led();
    if (settings_active()) {
        s_long_press_active = false;
        s_wake_front_button_pending = false;
        return;
    }
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
        queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_LONG_STOP);
    }
}

static esp_err_t init_button(void)
{
    const vibe_input_config_t config = {
        .front_long_ms = FRONT_PTT_LONG_PRESS_MS,
        .front_confirm_ms = BRIDGE_SELECTION_CONFIRM_HOLD_MS,
        .side_mode_ms = SIDE_MODE_TOGGLE_HOLD_MS,
        .side_calibration_ms = SIDE_MANUAL_CALIBRATION_HOLD_MS,
    };
    const vibe_input_callbacks_t callbacks = {
        .front_down = button_press_down_cb,
        .front_single = button_single_click_cb,
        .front_double = button_double_click_cb,
        .front_long = button_long_start_cb,
        .front_confirm = bridge_selection_confirm_long_cb,
        .front_up = button_up_cb,
        .side_up = side_button_up_cb,
        .side_single = side_button_single_click_cb,
        .side_double = side_button_double_click_cb,
        .side_mode_hold = side_button_long_start_cb,
        .side_calibration_hold = side_button_calibration_long_start_cb,
    };
    return vibe_input_init(&config, &callbacks);
}

#if defined(VIBE_BOARD_CARDPUTER_ADV)
static bool card_runtime_queue_command(
    vibe_app_command_t command,
    void *context)
{
    (void)context;
    return queue_event(command);
}

static void card_runtime_front_down(void *context)
{
    (void)context;
    button_press_down_cb(NULL, NULL);
}

static void card_runtime_front_up(void *context)
{
    (void)context;
    button_up_cb(NULL, NULL);
}

static void card_runtime_front_confirm(void *context)
{
    (void)context;
    bridge_selection_confirm_long_cb(NULL, NULL);
}

static void card_runtime_activity(void *context)
{
    (void)context;
    register_activity();
}

static char *card_setup_field_buffer(size_t *capacity)
{
    switch (s_card_setup_field) {
    case CARD_SETUP_WIFI_SSID:
        *capacity = sizeof(s_card_setup_draft.wifi_ssid);
        return s_card_setup_draft.wifi_ssid;
    case CARD_SETUP_WIFI_PASSWORD:
        *capacity = sizeof(s_card_setup_draft.wifi_password);
        return s_card_setup_draft.wifi_password;
    case CARD_SETUP_BRIDGE_HOST:
        *capacity = sizeof(s_card_setup_draft.bridge_host);
        return s_card_setup_draft.bridge_host;
    case CARD_SETUP_BRIDGE_PORT:
        *capacity = sizeof(s_card_setup_draft.bridge_port);
        return s_card_setup_draft.bridge_port;
    case CARD_SETUP_BRIDGE_TOKEN:
        *capacity = sizeof(s_card_setup_draft.bridge_token);
        return s_card_setup_draft.bridge_token;
    default:
        *capacity = 0;
        return NULL;
    }
}

static const char *card_setup_field_name(void)
{
    static const char *const names[CARD_SETUP_FIELD_COUNT] = {
        "WI-FI SSID",
        "WI-FI PASSWORD",
        "BRIDGE HOST",
        "BRIDGE PORT",
        "BRIDGE TOKEN",
    };
    return names[s_card_setup_field];
}

static void card_setup_render(void)
{
    if (!s_card_setup_active) {
        return;
    }
    size_t capacity = 0;
    const char *value = card_setup_field_buffer(&capacity);
    (void)capacity;
    char title[32];
    char display[40] = {0};
    snprintf(title, sizeof(title), "CONNECTION %u/%u",
             (unsigned)s_card_setup_field + 1,
             (unsigned)CARD_SETUP_FIELD_COUNT);
    if (s_card_setup_field == CARD_SETUP_WIFI_PASSWORD ||
        s_card_setup_field == CARD_SETUP_BRIDGE_TOKEN) {
        size_t length = value ? strlen(value) : 0;
        if (length > sizeof(display) - 2) {
            length = sizeof(display) - 2;
        }
        memset(display, '*', length);
        display[length] = '_';
        display[length + 1] = '\0';
    } else if (value && value[0] != '\0') {
        const size_t length = strlen(value);
        const char *visible = length > 28 ? value + length - 28 : value;
        snprintf(display, sizeof(display), "%.28s_", visible);
    } else {
        strlcpy(display, "(empty)_", sizeof(display));
    }

    const bool error = s_card_setup_error[0] != '\0';
    vibe_ui_card_setup(
        &s_ui,
        title,
        card_setup_field_name(),
        display,
        error ? s_card_setup_error
              : "Tab NEXT\nEnter SAVE\nFn+` CANCEL",
        error,
        true);
}

static void card_setup_close(void)
{
    s_card_setup_active = false;
    s_card_setup_error[0] = '\0';
    vibe_ui_card_setup(
        &s_ui, NULL, NULL, NULL, NULL, false, false);
    render_state();
}

static void card_setup_enter(void)
{
    if (s_card_setup_active) {
        return;
    }
    if (recording_network_busy() || ota_in_progress() || s_motion_calibrating ||
        atomic_load(&s_bridge_selection_active)) {
        (void)vibe_audio_play_sound(VIBE_STICK_SOUND_ERROR);
        return;
    }
    vibe_cardputer_runtime_stop_interactive(&s_card_runtime);

    memset(&s_card_setup_draft, 0, sizeof(s_card_setup_draft));
    vibe_wifi_profile_t current_wifi = {0};
    if (vibe_wifi_runtime_current_profile(&s_wifi, &current_wifi)) {
        strlcpy(s_card_setup_draft.wifi_ssid,
                current_wifi.ssid,
                sizeof(s_card_setup_draft.wifi_ssid));
        strlcpy(s_card_setup_draft.wifi_password,
                current_wifi.password,
                sizeof(s_card_setup_draft.wifi_password));
    }
    bridge_target_t target;
    bridge_profile_snapshot_t profile;
    bridge_target_copy(&target);
    if (bridge_target_profile_snapshot(&target, &profile)) {
        strlcpy(s_card_setup_draft.bridge_host, profile.host,
                sizeof(s_card_setup_draft.bridge_host));
        snprintf(s_card_setup_draft.bridge_port,
                 sizeof(s_card_setup_draft.bridge_port), "%d", profile.port);
        strlcpy(s_card_setup_draft.bridge_token, profile.token,
                sizeof(s_card_setup_draft.bridge_token));
    } else {
        strlcpy(s_card_setup_draft.bridge_host, target.host,
                sizeof(s_card_setup_draft.bridge_host));
        snprintf(s_card_setup_draft.bridge_port,
                 sizeof(s_card_setup_draft.bridge_port), "%d",
                 target.port > 0 ? target.port : VIBE_STICK_BRIDGE_PORT);
    }
    s_card_setup_field = CARD_SETUP_WIFI_SSID;
    s_card_setup_error[0] = '\0';
    s_card_setup_active = true;
    register_activity();
    card_setup_render();
    ESP_LOGI(TAG, "Cardputer connection setup entered");
}

static esp_err_t card_setup_store_manual_bridge(const char *ssid, int port)
{
    bridge_discovered_profile_t manual = {0};
    strlcpy(manual.id, CARD_SETUP_MANUAL_BRIDGE_ID, sizeof(manual.id));
    strlcpy(manual.label, "Cardputer Bridge", sizeof(manual.label));
    strlcpy(manual.host, s_card_setup_draft.bridge_host, sizeof(manual.host));
    manual.port = port;
    strlcpy(manual.token, s_card_setup_draft.bridge_token, sizeof(manual.token));
    return vibe_bridge_registry_upsert_manual(
        &s_bridge_registry, ssid, &manual, "keyboard");
}

static esp_err_t card_setup_save(void)
{
    if (s_card_setup_draft.wifi_ssid[0] == '\0') {
        strlcpy(s_card_setup_error, "WI-FI SSID REQUIRED", sizeof(s_card_setup_error));
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card_setup_draft.bridge_host[0] == '\0') {
        strlcpy(s_card_setup_error, "BRIDGE HOST REQUIRED", sizeof(s_card_setup_error));
        return ESP_ERR_INVALID_ARG;
    }
    char *end = NULL;
    long port_value = strtol(s_card_setup_draft.bridge_port, &end, 10);
    if (!end || *end != '\0' || port_value < 1 || port_value > 65535) {
        strlcpy(s_card_setup_error, "INVALID BRIDGE PORT", sizeof(s_card_setup_error));
        return ESP_ERR_INVALID_ARG;
    }

    vibe_wifi_profile_t candidate = {0};
    strlcpy(candidate.ssid, s_card_setup_draft.wifi_ssid,
            sizeof(candidate.ssid));
    strlcpy(candidate.password, s_card_setup_draft.wifi_password,
            sizeof(candidate.password));
    size_t profile_index = 0;
    esp_err_t wifi_store_err = vibe_wifi_runtime_store_profile(
        &s_wifi, &candidate, &profile_index);
    if (wifi_store_err == ESP_ERR_INVALID_ARG) {
        strlcpy(s_card_setup_error, "INVALID WI-FI PROFILE", sizeof(s_card_setup_error));
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(wifi_store_err, TAG, "save keyboard Wi-Fi");
    ESP_RETURN_ON_ERROR(card_setup_store_manual_bridge(
                            candidate.ssid, (int)port_value),
                        TAG, "save keyboard bridge");
    ESP_RETURN_ON_ERROR(
        vibe_wifi_runtime_connect_profile(&s_wifi, profile_index),
        TAG, "connect keyboard Wi-Fi");
    ESP_LOGI(TAG, "Cardputer connection setup saved ssid=%s bridge=%s:%ld",
             candidate.ssid, s_card_setup_draft.bridge_host, port_value);
    return ESP_OK;
}

static void card_setup_edit(const vibe_key_event_t *event)
{
    if (!event->pressed) {
        return;
    }
    s_card_setup_error[0] = '\0';
    register_activity();
    if (event->key == VIBE_KEY_ESCAPE) {
        card_setup_close();
        return;
    }
    if (event->key == VIBE_KEY_TAB || event->key == VIBE_KEY_DOWN ||
        event->key == VIBE_KEY_UP) {
        const bool previous = event->shift || event->key == VIBE_KEY_UP;
        s_card_setup_field = previous
                                 ? (card_setup_field_t)((s_card_setup_field +
                                                        CARD_SETUP_FIELD_COUNT - 1) %
                                                       CARD_SETUP_FIELD_COUNT)
                                 : (card_setup_field_t)((s_card_setup_field + 1) %
                                                       CARD_SETUP_FIELD_COUNT);
        card_setup_render();
        return;
    }
    if (event->key == VIBE_KEY_ENTER) {
        esp_err_t err = card_setup_save();
        if (err == ESP_OK) {
            card_setup_close();
            show_mode_switch_visual(
                "SETTINGS SAVED", "CONNECTING WI-FI",
                VIBE_UI_VISUAL_DONE, 0x86efac);
        } else {
            if (s_card_setup_error[0] == '\0') {
                snprintf(s_card_setup_error, sizeof(s_card_setup_error),
                         "SAVE FAILED: %s", esp_err_to_name(err));
            }
            card_setup_render();
        }
        return;
    }

    size_t capacity = 0;
    char *buffer = card_setup_field_buffer(&capacity);
    if (!buffer || capacity == 0) {
        return;
    }
    size_t length = strlen(buffer);
    if (event->key == VIBE_KEY_BACKSPACE || event->key == VIBE_KEY_DELETE) {
        if (length > 0) {
            buffer[length - 1] = '\0';
        }
        card_setup_render();
        return;
    }
    if (event->character >= 32 && event->character <= 126 &&
        length + 1 < capacity) {
        if (s_card_setup_field == CARD_SETUP_BRIDGE_PORT &&
            (event->character < '0' || event->character > '9')) {
            return;
        }
        buffer[length] = event->character;
        buffer[length + 1] = '\0';
        card_setup_render();
    }
}

static bool card_keyboard_report_has_keys(const card_keyboard_report_t *report)
{
    return report && (report->modifiers != 0 || report->key_count != 0);
}

static void card_keyboard_queue_current_report(void)
{
    if (!s_card_keyboard_report_queue) {
        return;
    }
    card_keyboard_report_t report = {
        .modifiers = s_card_host_modifiers,
    };
    for (size_t row = 0; row < 4 && report.key_count < sizeof(report.keys); ++row) {
        for (size_t col = 0; col < 14 && report.key_count < sizeof(report.keys); ++col) {
            if (s_card_host_usages[row][col] != 0) {
                report.keys[report.key_count++] = s_card_host_usages[row][col];
            }
        }
    }
    if (xQueueSend(s_card_keyboard_report_queue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cardputer keyboard report queue overflow; resyncing");
        xQueueReset(s_card_keyboard_report_queue);
        (void)xQueueSend(s_card_keyboard_report_queue, &report, 0);
    }
}

static void card_keyboard_release_host_state(void)
{
    bool changed = s_card_host_modifiers != 0;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 14; ++col) {
            changed |= s_card_host_usages[row][col] != 0;
        }
    }
    memset(s_card_host_usages, 0, sizeof(s_card_host_usages));
    s_card_host_modifiers = 0;
    if (changed) {
        card_keyboard_queue_current_report();
    }
}

static void card_keyboard_report_task(void *arg)
{
    (void)arg;
    char session_id[24];
    snprintf(session_id, sizeof(session_id), "%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    uint32_t sequence = 0;
    bool suspended_until_release = false;
    bool have_report = false;
    int64_t last_failure_log_ms = 0;
    card_keyboard_report_t current = {0};

    while (true) {
        card_keyboard_report_t next;
        const BaseType_t received = xQueueReceive(
            s_card_keyboard_report_queue, &next,
            pdMS_TO_TICKS(CARD_KEYBOARD_HEARTBEAT_MS));
        if (received == pdTRUE) {
            current = next;
            have_report = true;
        } else if (!card_keyboard_report_has_keys(&current)) {
            continue;
        }

        if (!wifi_connected()) {
            if (card_keyboard_report_has_keys(&current)) {
                suspended_until_release = true;
            }
            continue;
        }
        if (suspended_until_release) {
            if (card_keyboard_report_has_keys(&current)) {
                continue;
            }
            suspended_until_release = false;
        }
        if (!have_report && !card_keyboard_report_has_keys(&current)) {
            continue;
        }

        char keys_json[48] = {0};
        size_t used = 0;
        for (uint8_t i = 0; i < current.key_count; ++i) {
            int written = snprintf(keys_json + used, sizeof(keys_json) - used,
                                   "%s%u", i == 0 ? "" : ",",
                                   (unsigned)current.keys[i]);
            if (written < 0 || (size_t)written >= sizeof(keys_json) - used) {
                break;
            }
            used += (size_t)written;
        }
        char body[224];
        snprintf(body, sizeof(body),
                 "{\"protocol_version\":1,\"session_id\":\"%s\","
                 "\"sequence\":%lu,\"modifiers\":%u,\"keys\":[%s]}",
                 session_id, (unsigned long)sequence++,
                 (unsigned)current.modifiers, keys_json);
        char response[128] = {0};
        esp_err_t err = http_request_timeout(
            "POST", VIBE_STICK_DEVICE_KEYBOARD_REPORT_PATH, body,
            response, sizeof(response), CARD_KEYBOARD_REPORT_TIMEOUT_MS);
        if (err != ESP_OK) {
            if (card_keyboard_report_has_keys(&current)) {
                suspended_until_release = true;
            }
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_failure_log_ms >= 5000) {
                ESP_LOGW(TAG, "Cardputer keyboard report failed: %s",
                         esp_err_to_name(err));
                last_failure_log_ms = now_ms;
            }
        }
        have_report = false;
    }
}

static esp_err_t card_pointer_post(const char *path, const char *body,
                                   char *response, size_t response_len,
                                   int timeout_ms, void *context)
{
    (void)context;
    return http_request_timeout("POST", path, body, response,
                                (int)response_len, timeout_ms);
}

static bool card_pointer_online(void *context)
{
    (void)context;
    return wifi_connected();
}

static void card_post_mapped_input(const char *control, const char *phase)
{
    static uint32_t sequence;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"protocol_version\":1,\"session_id\":\"%08lx\","
             "\"sequence\":%lu,\"control\":\"%s\",\"phase\":\"%s\"}",
             (unsigned long)s_retained_boot_count,
             (unsigned long)sequence++, control, phase);
    char response[160] = {0};
    esp_err_t err = http_request_timeout(
        "POST", VIBE_STICK_DEVICE_INPUT_EVENT_PATH, body, response,
        sizeof(response), CARD_KEYBOARD_REPORT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mapped input delivery failed control=%s phase=%s: %s",
                 control, phase, esp_err_to_name(err));
        show_recording_overlay("HOST OFFLINE", "MAPPING UNAVAILABLE", true);
        vTaskDelay(pdMS_TO_TICKS(700));
        show_recording_overlay(NULL, NULL, false);
    }
}

static void card_dispatch_input_route(vibe_card_input_route_t route,
                                      const char *control,
                                      const char *phase)
{
    switch (route) {
    case VIBE_CARD_ROUTE_HOST:
        card_post_mapped_input(control, phase);
        break;
    case VIBE_CARD_ROUTE_DEVICE_RECORDING_TOGGLE:
        if (strcmp(phase, "trigger") == 0) {
            button_single_click_cb(NULL, NULL);
        }
        break;
    case VIBE_CARD_ROUTE_DEVICE_RECORDING_HOLD:
        if (strcmp(phase, "press") == 0) {
            button_press_down_cb(NULL, NULL);
            button_long_start_cb(NULL, NULL);
        } else if (strcmp(phase, "release") == 0) {
            button_up_cb(NULL, NULL);
        }
        break;
    case VIBE_CARD_ROUTE_DEVICE_LEGACY_DOUBLE:
        if (strcmp(phase, "trigger") == 0) {
            button_double_click_cb(NULL, NULL);
        }
        break;
    case VIBE_CARD_ROUTE_NONE:
    default:
        break;
    }
}

static bool card_air_mouse_busy(void *context)
{
    (void)context;
    return recording_network_busy() || ota_in_progress() ||
           settings_active() || s_motion_calibrating ||
           atomic_load(&s_bridge_selection_active);
}

static bool card_air_mouse_keep_motion_active(void *context)
{
    (void)context;
    return s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK;
}

static void card_air_mouse_release_keyboard(void *context)
{
    (void)context;
    card_keyboard_release_host_state();
}

static void card_air_mouse_activity(void *context)
{
    (void)context;
    register_activity();
}

static void card_air_mouse_status(vibe_card_air_mouse_status_t status,
                                  uint16_t progress, void *context)
{
    (void)context;
    const char *title = "AIR MOUSE";
    const char *detail = "FN+M TO START";
    vibe_ui_visual_t visual = VIBE_UI_VISUAL_DICTATION;
    uint32_t color = 0x8a9099;
    char progress_text[32];

    switch (status) {
    case VIBE_CARD_AIR_MOUSE_DISABLED:
        title = "AIR MOUSE OFF";
        break;
    case VIBE_CARD_AIR_MOUSE_CALIBRATING:
        snprintf(progress_text, sizeof(progress_text), "KEEP STILL %u/%u",
                 (unsigned)progress,
                 (unsigned)VIBE_AIR_MOUSE_CALIBRATION_SAMPLES);
        detail = progress_text;
        color = 0x93c5fd;
        break;
    case VIBE_CARD_AIR_MOUSE_READY:
        detail = "SPACE LEFT  ENTER RIGHT";
        visual = VIBE_UI_VISUAL_DONE;
        color = 0x86efac;
        break;
    case VIBE_CARD_AIR_MOUSE_BUSY:
        detail = "BUSY";
        visual = VIBE_UI_VISUAL_ERROR;
        color = 0xfca5a5;
        break;
    case VIBE_CARD_AIR_MOUSE_UNAVAILABLE:
        detail = "IMU UNAVAILABLE";
        visual = VIBE_UI_VISUAL_ERROR;
        color = 0xfca5a5;
        (void)vibe_audio_play_sound(VIBE_STICK_SOUND_ERROR);
        break;
    case VIBE_CARD_AIR_MOUSE_FAILED:
        detail = "IMU FAILED";
        visual = VIBE_UI_VISUAL_ERROR;
        color = 0xfca5a5;
        (void)vibe_audio_play_sound(VIBE_STICK_SOUND_ERROR);
        break;
    }
    show_mode_switch_visual(title, detail, visual, color);
}

static void card_keyboard_event(const vibe_key_event_t *event, void *context)
{
    (void)context;
    if (!event) {
        return;
    }
    const uint8_t row = event->row;
    const uint8_t col = event->column;
    if (row >= 4 || col >= 14) {
        return;
    }
    /* Opt is Cardputer's physical recording button. Keep it independent from
     * page-level keyboard handlers, matching the front button on StickS3 and
     * StickC Plus. Setup keeps its existing local handling below. */
    if (!s_card_setup_active && event->key == VIBE_KEY_OPT) {
        if (vibe_cardputer_runtime_messages_busy(&s_card_runtime)) {
            vibe_cardputer_runtime_release_display_resources(&s_card_runtime);
        }
        if (event->pressed) {
            vibe_cardputer_runtime_opt_press(&s_card_runtime);
        } else {
            if (vibe_cardputer_runtime_opt_chord_active(
                    &s_card_runtime)) {
                s_card_host_modifiers = event->hid_modifiers;
                card_keyboard_queue_current_report();
            }
            vibe_cardputer_runtime_opt_release(&s_card_runtime);
        }
        return;
    }
    const vibe_cardputer_key_result_t feature_result =
        vibe_cardputer_runtime_handle_feature_key(
            &s_card_runtime, event);
    if (feature_result ==
        VIBE_CARDPUTER_KEY_CONSUMED_RELEASE_KEYBOARD) {
        card_keyboard_release_host_state();
        return;
    }
    if (feature_result == VIBE_CARDPUTER_KEY_CONSUMED) {
        return;
    }

    if (s_card_setup_active || s_card_local_consumed[row][col]) {
        const bool edit_active = s_card_setup_active;
        if (event->pressed) {
            s_card_local_consumed[row][col] = true;
        }
        if (edit_active) {
            card_setup_edit(event);
        }
        if (event->key == VIBE_KEY_OPT && !event->pressed) {
            vibe_cardputer_runtime_opt_release(&s_card_runtime);
        }
        if (!event->pressed) {
            s_card_local_consumed[row][col] = false;
        }
        card_keyboard_release_host_state();
        return;
    }

    if (event->key == VIBE_KEY_FN) {
        return;
    }

    if (event->pressed &&
        !vibe_cardputer_runtime_opt_mark_chord(
            &s_card_runtime)) {
        s_card_local_consumed[row][col] = true;
        return;
    }
    if (event->pressed && event->fn) {
        if (row == 2 && col == 3) { // Official Fn+S has no HID value.
            s_card_local_consumed[row][col] = true;
            card_keyboard_release_host_state();
            card_setup_enter();
            return;
        }
    }
    if (event->pressed) {
        register_activity();
    }

    s_card_host_modifiers = event->hid_modifiers;
    if (event->hid_usage != 0) {
        s_card_host_usages[row][col] = event->pressed ? event->hid_usage : 0;
    }
    card_keyboard_queue_current_report();
}

#endif

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
    const vibe_motion_controller_command_t command = {
        .type = VIBE_MOTION_COMMAND_BEGIN_WAKE_CONFIRM,
        .data.now_ms = esp_timer_get_time() / 1000,
    };
    (void)vibe_motion_controller_handle(
        &s_motion_controller, &command);
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
    queue_input_signal(VIBE_INPUT_SIGNAL_FRONT_LONG_START);
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
    if (!s_motion_controller.state.calibration_timeout_due) {
        return;
    }

    if (s_motion_calibration_had_previous &&
        vibe_motion_apply_calibration(&s_motion_previous_calibration) == ESP_OK) {
        s_motion_calibrating = false;
        s_motion_calibration_deadline_ms = 0;
        s_motion_calibration_had_previous = false;
        s_motion_lift_armed = false;
        s_motion_start_pending = false;
        ESP_LOGW(TAG, "lift calibration timed out; restored previous calibration");
        render_state();
        show_mode_switch_visual("CAL FAILED", "LIFT RESTORED",
                                VIBE_UI_VISUAL_DICTATION,
                                0xfacc15);
        return;
    }

    ESP_LOGW(TAG, "lift calibration timed out without fallback; returning to PTT");
    set_push_to_talk_trigger_mode();
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_recording_mode_preference());
    render_state();
    show_mode_switch_visual("CAL FAILED", "PTT",
                            VIBE_UI_VISUAL_DICTATION,
                            0xfca5a5);
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
    if (
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        vibe_cardputer_runtime_air_mouse_enabled(&s_card_runtime) ||
#endif
        s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK ||
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
    vibe_app_event_t event;
    int64_t last_poll = 0;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        vibe_recording_controller_tick(&s_recording, now_ms);
        vibe_motion_controller_tick(&s_motion_controller, now_ms);
        maybe_refresh_power_status(now_ms);
        maybe_timeout_cyber_tts_wait(now_ms);
        maybe_timeout_motion_calibration(now_ms);
        maybe_timeout_settings(now_ms);
        maybe_advance_bridge_selection_visual(now_ms);
        update_power_saving(now_ms);
        vibe_ota_runtime_tick(&s_ota, now_ms);
        maybe_enter_deep_sleep(now_ms);
        handle_deep_sleep_front_button_intent();
        poll_front_button_fallback(now_ms);
        if (s_recording_local_capture &&
            atomic_load(&s_recording_session_active) &&
            vibe_recording_upload_failed() &&
            !recording_finalize_active() &&
            !s_recording_upload_abort_requested) {
            const vibe_recording_command_t abort_command = {
                .type = VIBE_RECORDING_COMMAND_REQUEST_UPLOAD_ABORT,
            };
            (void)vibe_recording_controller_handle(
                &s_recording, &abort_command);
            ESP_LOGW(TAG, "recording stopped automatically after audio upload failure");
            handle_recording_stop("audio_upload_failed");
        }
        const bool network_busy = recording_network_busy();
        if (s_display_power_state == DISPLAY_POWER_ACTIVE &&
            wifi_connected() && !network_busy &&
            now_ms - last_poll >= state_poll_interval_ms(now_ms)) {
            last_poll = now_ms;
            poll_state();
        }
        if (s_motion_controller.state.network_timeout_due) {
            ESP_LOGW(TAG, "motion lift cancelled: Wi-Fi did not connect within %dms",
                     VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS);
            s_motion_wake_network_pending = false;
            s_motion_wake_network_deadline_ms = 0;
            s_motion_start_pending = false;
            s_motion_lift_armed = false;
            show_recording_overlay("CONNECT FAILED", "", true);
            vTaskDelay(pdMS_TO_TICKS(700));
            show_recording_overlay(NULL, NULL, false);
        }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        if (vibe_cardputer_runtime_air_mouse_enabled(
                &s_card_runtime)) {
            vibe_cardputer_runtime_tick(&s_card_runtime, now_ms);
        } else
        #endif
        if (!settings_active() && vibe_motion_available() &&
            s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK) {
            vibe_motion_event_t motion_event = vibe_motion_poll(now_ms);
            if (s_motion_calibrating && !vibe_motion_is_calibrating()) {
                s_motion_calibrating = false;
                s_motion_calibration_deadline_ms = 0;
                s_motion_calibration_had_previous = false;
                s_motion_lift_armed = true;
                s_motion_start_pending = false;
                set_motion_arm_prompt(false);
                ESP_ERROR_CHECK_WITHOUT_ABORT(save_motion_calibration());
                ESP_LOGI(TAG, "lift recording mode calibration complete");
                render_state();
            }
            const bool motion_arm_idle =
                !s_motion_calibrating &&
                !s_motion_wake_confirm_pending &&
                !s_motion_wake_network_pending &&
                !s_motion_start_pending &&
                !s_motion_recording_active &&
                !recording_finalize_active();
            if (motion_arm_idle && !s_motion_lift_armed &&
                vibe_motion_is_flat_stable()) {
                s_motion_lift_armed = true;
                ESP_LOGI(TAG, "lift recording armed after stable flat posture");
            }
            set_motion_arm_prompt(motion_arm_idle && !s_motion_lift_armed);
            bool motion_wake_handled = false;
            if (!s_motion_calibrating && s_motion_wake_confirm_pending) {
                motion_wake_handled = true;
                if (s_motion_controller.state.wake_confirm_due) {
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
                    (void)queue_input_signal(VIBE_INPUT_SIGNAL_MOTION_FLAT);
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
        case VIBE_STICK_EVENT_SETTINGS_ENTER:
            enter_settings();
            break;
        case VIBE_STICK_EVENT_SETTINGS_PAGE_NEXT:
            next_settings_page();
            break;
        case VIBE_STICK_EVENT_SETTINGS_VALUE_NEXT:
            next_settings_value();
            break;
        case VIBE_STICK_EVENT_SETTINGS_CONFIRM:
            confirm_settings();
            break;
        case VIBE_STICK_EVENT_MOTION_START:
            if (s_recording_trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK &&
                !s_motion_recording_active) {
                s_motion_start_pending = false;
                recording_set_motion_active(
                    handle_recording_start(
                        "motion_lift_start", "PLACE DOWN"));
                if (!s_motion_recording_active) {
                    s_motion_lift_armed = false;
                }
            }
            break;
        case VIBE_STICK_EVENT_MOTION_STOP:
            if (s_motion_recording_active) {
                handle_recording_stop("motion_lift_stop");
                recording_set_motion_active(false);
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
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        case VIBE_STICK_EVENT_CARD_OPT_TAP:
            card_dispatch_input_route(vibe_cardputer_runtime_route(
                                          &s_card_runtime,
                                          VIBE_CARDPUTER_OPT_TAP),
                                      "cardputer.opt.tap", "trigger");
            if (!queue_event(VIBE_STICK_EVENT_CARD_OPT_COMPLETE)) {
                vibe_cardputer_runtime_opt_action_complete(
                    &s_card_runtime);
            }
            break;
        case VIBE_STICK_EVENT_CARD_OPT_DOUBLE:
            card_dispatch_input_route(vibe_cardputer_runtime_route(
                                          &s_card_runtime,
                                          VIBE_CARDPUTER_OPT_DOUBLE),
                                      "cardputer.opt.double", "trigger");
            if (!queue_event(VIBE_STICK_EVENT_CARD_OPT_COMPLETE)) {
                vibe_cardputer_runtime_opt_action_complete(
                    &s_card_runtime);
            }
            break;
        case VIBE_STICK_EVENT_CARD_OPT_HOLD_START:
            card_dispatch_input_route(
                vibe_cardputer_runtime_opt_active_hold_route(
                    &s_card_runtime),
                                      "cardputer.opt.hold", "press");
            if (!queue_event(VIBE_STICK_EVENT_CARD_OPT_COMPLETE)) {
                vibe_cardputer_runtime_opt_action_complete(
                    &s_card_runtime);
            }
            break;
        case VIBE_STICK_EVENT_CARD_OPT_HOLD_STOP:
            card_dispatch_input_route(
                vibe_cardputer_runtime_opt_active_hold_route(
                    &s_card_runtime),
                                      "cardputer.opt.hold", "release");
            if (!queue_event(VIBE_STICK_EVENT_CARD_OPT_COMPLETE)) {
                vibe_cardputer_runtime_opt_action_complete(
                    &s_card_runtime);
            }
            break;
        case VIBE_STICK_EVENT_CARD_OPT_COMPLETE:
            vibe_cardputer_runtime_opt_action_complete(
                &s_card_runtime);
            break;
#endif
        case VIBE_STICK_EVENT_OTA_CHECK:
            start_ota_check_task();
            break;
        case VIBE_APP_COMMAND_NONE:
        default:
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
            ESP_LOGI(TAG, "serial debug command: side double-click full scan");
            side_button_double_click_cb(NULL, NULL);
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

void vibe_app_runtime_start(void)
{
    const vibe_recording_controller_config_t recording_config = {
        .cyber_intents = VIBE_BOARD_HAS_CYBER_INTENTS,
    };
    const vibe_recording_controller_dependencies_t recording_dependencies = {
        .context = NULL,
    };
    vibe_recording_controller_init(
        &s_recording, &recording_config, &recording_dependencies);
    const vibe_motion_controller_config_t motion_config = {
        .calibration_timeout_ms =
            VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS,
        .wake_confirm_ms = VIBE_STICK_MOTION_WAKE_CONFIRM_MS,
        .network_timeout_ms =
            VIBE_STICK_MOTION_WAKE_NETWORK_TIMEOUT_MS,
    };
    const vibe_motion_controller_dependencies_t motion_dependencies = {
        .context = NULL,
    };
    vibe_motion_controller_init(
        &s_motion_controller, &motion_config, &motion_dependencies);
    const vibe_power_runtime_config_t power_config = {
        .dim_after_ms = VIBE_STICK_IDLE_DIM_MS,
        .off_after_ms = VIBE_STICK_IDLE_OFF_MS,
        .usb_unplug_hold_ms =
            VIBE_STICK_BATTERY_USB_UNPLUG_HOLD_MS,
        .wake_stabilize_ms =
            VIBE_STICK_BATTERY_WAKE_STABILIZE_MS,
    };
    const vibe_power_runtime_dependencies_t power_dependencies = {
        .active_work = power_active_work,
        .false_wake_due = power_false_wake_due,
        .context = NULL,
    };
    vibe_power_runtime_init(
        &s_power, &power_config, &power_dependencies,
        LCD_BACKLIGHT_DEFAULT);
    vibe_app_state_init(&s_state);
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
        const gpio_num_t wake_gpio = sleep_button_wake_gpio();
        if (wake_gpio != GPIO_NUM_NC) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_deinit(wake_gpio));
        }
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        if ((s_boot_ext1_wake_status & cardputer_keyboard_wake_mask()) != 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                rtc_gpio_deinit(VIBE_BOARD_PIN_KEYBOARD_INT));
        }
#endif
#if VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE
        if ((s_boot_ext1_wake_status &
             (1ULL << VIBE_BOARD_PIN_MOTION_WAKE)) != 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                rtc_gpio_deinit(VIBE_BOARD_PIN_MOTION_WAKE));
        }
#endif
    }
    if (s_woke_from_deep_sleep) {
        const vibe_power_runtime_command_t wake_command = {
            .type = VIBE_POWER_COMMAND_SET_WAKE_TIME,
            .data.time_ms = esp_timer_get_time() / 1000,
        };
        (void)vibe_power_runtime_handle(&s_power, &wake_command);
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
    log_persisted_recording_diagnostic();
    ESP_ERROR_CHECK_WITHOUT_ABORT(load_deep_sleep_record());
    ESP_ERROR_CHECK_WITHOUT_ABORT(restore_sleep_preference());

    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_init_power());
    ESP_ERROR_CHECK_WITHOUT_ABORT(vibe_board_status_led_set(false));
    s_status_led_on = false;
    s_boot_power_status = vibe_board_boot_power_status();
    s_event_queue = xQueueCreate(16, sizeof(vibe_app_event_t));
    s_bridge_control_queue =
        xQueueCreate(8, sizeof(bridge_control_command_t));
    ESP_ERROR_CHECK(s_event_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_bridge_control_queue ? ESP_OK : ESP_ERR_NO_MEM);
    const vibe_bridge_registry_config_t bridge_registry_config = {
        .configured_profiles = k_configured_bridge_profiles,
        .configured_profile_count =
            sizeof(k_configured_bridge_profiles) /
            sizeof(k_configured_bridge_profiles[0]),
        .default_profile_id = VIBE_STICK_DEFAULT_BRIDGE_ID,
        .default_host = VIBE_STICK_DEFAULT_BRIDGE_HOST,
        .default_port = VIBE_STICK_DEFAULT_BRIDGE_PORT,
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        .failure_threshold = 3,
#else
        .failure_threshold = 1,
#endif
        .current_ssid = bridge_registry_current_ssid,
    };
    ESP_ERROR_CHECK(vibe_bridge_registry_init(
        &s_bridge_registry, &bridge_registry_config));
    const vibe_bridge_client_config_t bridge_client_config = {
        .rx_buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .tx_buffer_size = HTTP_CLIENT_TX_BUFFER_SIZE,
        .set_headers = bridge_client_set_headers,
    };
    ESP_ERROR_CHECK(vibe_bridge_client_init(
        &s_bridge_client, &bridge_client_config));
    const vibe_ota_runtime_config_t ota_config = {
        .board = VIBE_BOARD_NAME,
        .version = FIRMWARE_VERSION,
        .build_id = FIRMWARE_BUILD_ID,
        .manifest_path = VIBE_STICK_OTA_MANIFEST_PATH,
        .binary_path = VIBE_STICK_OTA_BIN_PATH,
        .rx_buffer_size = HTTP_CLIENT_RX_BUFFER_SIZE,
        .tx_buffer_size = HTTP_CLIENT_TX_BUFFER_SIZE,
        .read_buffer_bytes = OTA_READ_BUFFER_BYTES,
        .download_timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .no_progress_timeout_ms = OTA_NO_PROGRESS_TIMEOUT_MS,
        .periodic_check_ms = OTA_PERIODIC_CHECK_MS,
        .battery_check_ms = OTA_BATTERY_CHECK_MS,
#if defined(VIBE_BOARD_CARDPUTER_ADV)
        .task_stack_bytes = 6144,
#else
        .task_stack_bytes = 8192,
#endif
        .task_priority = 3,
        .task_core = VIBE_STICK_NETWORK_CORE,
        .enabled = VIBE_STICK_OTA_ENABLED,
    };
    const vibe_ota_runtime_dependencies_t ota_dependencies = {
        .request_manifest = ota_request_manifest,
        .build_url = ota_build_url,
        .prepare_download = ota_prepare_download,
        .prepare_client = ota_prepare_client,
        .online = ota_online,
        .busy = ota_busy,
        .external_powered = ota_external_powered,
        .display_active = ota_display_active,
        .settings_active = ota_settings_active,
        .overlay = ota_overlay,
        .before_start = ota_before_start,
        .context = NULL,
    };
    ESP_ERROR_CHECK(vibe_ota_runtime_init(
        &s_ota, &ota_config, &ota_dependencies));
#if VIBE_STICK_SERIAL_DEBUG_ENABLED
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    xTaskCreate(serial_debug_task, "serial_debug", 2048, NULL, 2, NULL);
#else
    xTaskCreate(serial_debug_task, "serial_debug", 6144, NULL, 2, NULL);
#endif
#endif
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(init_display());
    create_ui();
    s_ui_ready = true;
    register_activity();
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
    if (s_sleep_minutes == VIBE_SETTINGS_SLEEP_DISABLED_MINUTES) {
        ESP_LOGI(TAG, "deep sleep disabled by preference");
    } else {
        ESP_LOGI(TAG, "deep sleep timeout=%umin", (unsigned)s_sleep_minutes);
    }
    render_state();
    BaseType_t bridge_control_ok =
        xTaskCreatePinnedToCore(bridge_control_task, "bridge_control", 4096,
                                NULL, 5, NULL, VIBE_STICK_APP_CORE);
    ESP_ERROR_CHECK(bridge_control_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(init_button());
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    s_card_keyboard_report_queue = xQueueCreate(
        CARD_KEYBOARD_REPORT_QUEUE_LENGTH, sizeof(card_keyboard_report_t));
    ESP_ERROR_CHECK(s_card_keyboard_report_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t keyboard_report_ok = xTaskCreatePinnedToCore(
        card_keyboard_report_task, "keyboard_report", 6144, NULL, 3, NULL,
        VIBE_STICK_NETWORK_CORE);
    ESP_ERROR_CHECK(keyboard_report_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const vibe_cardputer_runtime_config_t card_runtime_config = {
        .air_mouse = {
            .pointer = {
                .path = VIBE_STICK_DEVICE_POINTER_REPORT_PATH,
                .timeout_ms = CARD_POINTER_REPORT_TIMEOUT_MS,
                .heartbeat_ms = CARD_POINTER_HEARTBEAT_MS,
                .post = card_pointer_post,
                .online = card_pointer_online,
            },
            .busy = card_air_mouse_busy,
            .keep_motion_active = card_air_mouse_keep_motion_active,
            .release_keyboard = card_air_mouse_release_keyboard,
            .activity = card_air_mouse_activity,
            .status = card_air_mouse_status,
        },
        .volume = {
            .display_lock = lvgl_lock,
            .display_unlock = lvgl_unlock,
            .activity = card_message_activity,
        },
        .messages = {
            .request = card_message_request,
            .download = card_message_download,
            .display_lock = lvgl_lock,
            .display_unlock = lvgl_unlock,
            .set_landscape = card_message_set_landscape,
            .restore_home = render_state,
            .activity = card_message_activity,
            .audio_busy = card_message_busy,
        },
        .keyboard_callback = card_keyboard_event,
        .keyboard_context = NULL,
        .queue_command = card_runtime_queue_command,
        .front_down = card_runtime_front_down,
        .front_up = card_runtime_front_up,
        .front_confirm = card_runtime_front_confirm,
        .activity = card_runtime_activity,
        .enable_air_mouse = false,
        .enable_messages = false,
        .opt_click_window_ms = CARD_OPT_CLICK_WINDOW_MS,
        .opt_long_press_ms = FRONT_PTT_LONG_PRESS_MS,
        .opt_confirm_hold_ms = BRIDGE_SELECTION_CONFIRM_HOLD_MS,
        .context = NULL,
    };
    ESP_ERROR_CHECK(vibe_cardputer_runtime_init(
        &s_card_runtime, &card_runtime_config));
#endif
    capture_deep_sleep_front_button_intent();
    capture_deep_sleep_motion_intent();
    ESP_ERROR_CHECK(vibe_audio_init());
#if defined(VIBE_BOARD_CARDPUTER_ADV)
    /* Voice-priority Cardputer build: do not start SD message sync. */
#endif
    BaseType_t command_task_ok =
        xTaskCreatePinnedToCore(device_command_task, "device_commands", 8192,
                                NULL, 3, NULL, VIBE_STICK_NETWORK_CORE);
    ESP_ERROR_CHECK(command_task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreatePinnedToCore(app_task, "agent_app", 6144, NULL, 4, NULL,
                            VIBE_STICK_APP_CORE);
}
