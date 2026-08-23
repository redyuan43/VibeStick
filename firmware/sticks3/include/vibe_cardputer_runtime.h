#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_err.h"
#include "vibe_app_command.h"
#include "vibe_cardputer_air_mouse.h"
#include "vibe_cardputer_input_profile.h"
#include "vibe_cardputer_messages.h"
#include "vibe_cardputer_volume.h"
#include "vibe_keyboard.h"

typedef enum {
    VIBE_CARDPUTER_KEY_NOT_HANDLED = 0,
    VIBE_CARDPUTER_KEY_CONSUMED,
    VIBE_CARDPUTER_KEY_CONSUMED_RELEASE_KEYBOARD,
} vibe_cardputer_key_result_t;

typedef enum {
    VIBE_CARDPUTER_OPT_TAP = 0,
    VIBE_CARDPUTER_OPT_DOUBLE,
    VIBE_CARDPUTER_OPT_HOLD,
} vibe_cardputer_opt_gesture_t;

typedef bool (*vibe_cardputer_queue_command_fn)(
    vibe_app_command_t command,
    void *context);
typedef void (*vibe_cardputer_action_fn)(void *context);

typedef struct {
    vibe_card_air_mouse_config_t air_mouse;
    vibe_cardputer_volume_config_t volume;
    vibe_cardputer_messages_config_t messages;
    vibe_keyboard_callback_t keyboard_callback;
    void *keyboard_context;
    vibe_cardputer_queue_command_fn queue_command;
    vibe_cardputer_action_fn front_down;
    vibe_cardputer_action_fn front_up;
    vibe_cardputer_action_fn front_confirm;
    vibe_cardputer_action_fn activity;
    bool enable_air_mouse;
    bool enable_messages;
    uint32_t opt_click_window_ms;
    uint32_t opt_long_press_ms;
    uint32_t opt_confirm_hold_ms;
    void *context;
} vibe_cardputer_runtime_config_t;

typedef struct {
    vibe_card_input_profile_t input_profile;
    bool air_mouse_enabled;
    bool messages_busy;
    bool message_storage_ready;
} vibe_cardputer_runtime_snapshot_t;

typedef struct {
    vibe_cardputer_runtime_config_t config;
    vibe_card_input_profile_t input_profile;
    void *opt_long_timer;
    void *opt_confirm_timer;
    void *opt_click_timer;
    void *profile_lock;
    atomic_bool opt_down;
    atomic_bool opt_chord;
    atomic_bool opt_button_committed;
    atomic_int opt_pending_clicks;
    atomic_uint opt_actions_pending;
    atomic_uint profile_revision;
    atomic_int opt_tap_route;
    atomic_int opt_double_route;
    atomic_int opt_hold_route;
    atomic_int opt_active_hold_route;
    bool initialized;
    bool messages_started;
} vibe_cardputer_runtime_t;

esp_err_t vibe_cardputer_runtime_init(
    vibe_cardputer_runtime_t *runtime,
    const vibe_cardputer_runtime_config_t *config);
esp_err_t vibe_cardputer_runtime_start_messages(
    vibe_cardputer_runtime_t *runtime);
vibe_cardputer_key_result_t vibe_cardputer_runtime_handle_feature_key(
    vibe_cardputer_runtime_t *runtime,
    const vibe_key_event_t *event);
esp_err_t vibe_cardputer_runtime_apply_profile(
    vibe_cardputer_runtime_t *runtime,
    const vibe_card_input_profile_t *profile);
void vibe_cardputer_runtime_tick(
    vibe_cardputer_runtime_t *runtime,
    int64_t now_ms);
void vibe_cardputer_runtime_stop_interactive(
    vibe_cardputer_runtime_t *runtime);
void vibe_cardputer_runtime_release_display_resources(
    vibe_cardputer_runtime_t *runtime);
void vibe_cardputer_runtime_opt_press(
    vibe_cardputer_runtime_t *runtime);
void vibe_cardputer_runtime_opt_release(
    vibe_cardputer_runtime_t *runtime);
bool vibe_cardputer_runtime_opt_mark_chord(
    vibe_cardputer_runtime_t *runtime);
bool vibe_cardputer_runtime_opt_chord_active(
    const vibe_cardputer_runtime_t *runtime);
bool vibe_cardputer_runtime_opt_busy(
    const vibe_cardputer_runtime_t *runtime);
void vibe_cardputer_runtime_opt_action_complete(
    vibe_cardputer_runtime_t *runtime);
vibe_card_input_route_t
vibe_cardputer_runtime_opt_active_hold_route(
    const vibe_cardputer_runtime_t *runtime);
bool vibe_cardputer_runtime_messages_busy(
    const vibe_cardputer_runtime_t *runtime);
bool vibe_cardputer_runtime_air_mouse_enabled(
    const vibe_cardputer_runtime_t *runtime);
uint32_t vibe_cardputer_runtime_profile_revision(
    const vibe_cardputer_runtime_t *runtime);
vibe_card_input_route_t vibe_cardputer_runtime_route(
    const vibe_cardputer_runtime_t *runtime,
    vibe_cardputer_opt_gesture_t opt_gesture);
void vibe_cardputer_runtime_snapshot(
    const vibe_cardputer_runtime_t *runtime,
    vibe_cardputer_runtime_snapshot_t *snapshot);
