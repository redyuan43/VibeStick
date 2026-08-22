#include "vibe_cardputer_runtime.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "vibe_input_router.h"

static const char *TAG = "card_runtime";

static SemaphoreHandle_t profile_lock(
    const vibe_cardputer_runtime_t *runtime)
{
    return (SemaphoreHandle_t)runtime->profile_lock;
}

static void lock_profile(
    const vibe_cardputer_runtime_t *runtime)
{
    xSemaphoreTake(profile_lock(runtime), portMAX_DELAY);
}

static void unlock_profile(
    const vibe_cardputer_runtime_t *runtime)
{
    xSemaphoreGive(profile_lock(runtime));
}

static esp_timer_handle_t opt_timer(void *timer)
{
    return (esp_timer_handle_t)timer;
}

static void stop_opt_timer(void *timer)
{
    if (timer) {
        (void)esp_timer_stop(opt_timer(timer));
    }
}

static void cancel_opt_hold_timers(
    vibe_cardputer_runtime_t *runtime)
{
    stop_opt_timer(runtime->opt_long_timer);
    stop_opt_timer(runtime->opt_confirm_timer);
}

static bool queue_opt_command(
    vibe_cardputer_runtime_t *runtime,
    vibe_app_command_t command)
{
    atomic_fetch_add(&runtime->opt_actions_pending, 1);
    if (!runtime->config.queue_command ||
        !runtime->config.queue_command(
            command, runtime->config.context)) {
        atomic_fetch_sub(&runtime->opt_actions_pending, 1);
        ESP_LOGW(
            TAG, "Opt command queue full type=%d",
            (int)command);
        return false;
    }
    return true;
}

static void opt_long_timer_cb(void *arg)
{
    vibe_cardputer_runtime_t *runtime = arg;
    if (!runtime ||
        !atomic_load(&runtime->opt_down) ||
        atomic_load(&runtime->opt_chord)) {
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(
            &runtime->opt_button_committed,
            &expected, true)) {
        return;
    }
    atomic_store(
        &runtime->opt_active_hold_route,
        atomic_load(&runtime->opt_hold_route));
    (void)queue_opt_command(
        runtime,
        vibe_input_router_command(
            VIBE_INPUT_SIGNAL_CARD_OPT_HOLD_START));
}

static void opt_confirm_timer_cb(void *arg)
{
    vibe_cardputer_runtime_t *runtime = arg;
    if (!runtime ||
        !atomic_load(&runtime->opt_down) ||
        atomic_load(&runtime->opt_chord)) {
        return;
    }
    bool expected = false;
    if (atomic_compare_exchange_strong(
            &runtime->opt_button_committed,
            &expected, true)) {
        if (runtime->config.front_down) {
            runtime->config.front_down(runtime->config.context);
        }
    }
    if (runtime->config.front_confirm) {
        runtime->config.front_confirm(runtime->config.context);
    }
}

static void opt_click_timer_cb(void *arg)
{
    vibe_cardputer_runtime_t *runtime = arg;
    if (runtime &&
        atomic_exchange(
            &runtime->opt_pending_clicks, 0) == 1) {
        (void)queue_opt_command(
            runtime,
            vibe_input_router_command(
                VIBE_INPUT_SIGNAL_CARD_OPT_TAP));
    }
}

static esp_err_t init_opt_timers(
    vibe_cardputer_runtime_t *runtime)
{
    const esp_timer_create_args_t long_args = {
        .callback = opt_long_timer_cb,
        .arg = runtime,
        .name = "card_opt_long",
        .skip_unhandled_events = true,
    };
    const esp_timer_create_args_t confirm_args = {
        .callback = opt_confirm_timer_cb,
        .arg = runtime,
        .name = "card_opt_confirm",
        .skip_unhandled_events = true,
    };
    const esp_timer_create_args_t click_args = {
        .callback = opt_click_timer_cb,
        .arg = runtime,
        .name = "card_opt_click",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = NULL;
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&long_args, &timer),
        TAG, "create Opt long timer");
    runtime->opt_long_timer = timer;
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&confirm_args, &timer),
        TAG, "create Opt confirm timer");
    runtime->opt_confirm_timer = timer;
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&click_args, &timer),
        TAG, "create Opt click timer");
    runtime->opt_click_timer = timer;
    return ESP_OK;
}

esp_err_t vibe_cardputer_runtime_init(
    vibe_cardputer_runtime_t *runtime,
    const vibe_cardputer_runtime_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        runtime && config && config->keyboard_callback &&
            config->queue_command &&
            config->opt_click_window_ms > 0 &&
            config->opt_long_press_ms > 0 &&
            config->opt_confirm_hold_ms > 0,
        ESP_ERR_INVALID_ARG, TAG, "missing runtime config");

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    runtime->profile_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(
        runtime->profile_lock, ESP_ERR_NO_MEM,
        TAG, "create profile lock");
    atomic_init(&runtime->opt_down, false);
    atomic_init(&runtime->opt_chord, false);
    atomic_init(&runtime->opt_button_committed, false);
    atomic_init(&runtime->opt_pending_clicks, 0);
    atomic_init(&runtime->opt_actions_pending, 0);

    ESP_RETURN_ON_ERROR(
        vibe_cardputer_air_mouse_init(&config->air_mouse),
        TAG, "initialize air mouse");

    vibe_card_input_profile_default(&runtime->input_profile);
    esp_err_t profile_err =
        vibe_card_input_profile_load(&runtime->input_profile);
    if (profile_err != ESP_OK &&
        profile_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(
            TAG, "input profile load skipped: %s",
            esp_err_to_name(profile_err));
        vibe_card_input_profile_default(&runtime->input_profile);
    }
    atomic_init(
        &runtime->profile_revision,
        runtime->input_profile.revision);
    atomic_init(
        &runtime->opt_tap_route,
        runtime->input_profile.opt_tap);
    atomic_init(
        &runtime->opt_double_route,
        runtime->input_profile.opt_double);
    atomic_init(
        &runtime->opt_hold_route,
        runtime->input_profile.opt_hold);
    atomic_init(
        &runtime->opt_active_hold_route,
        VIBE_CARD_ROUTE_NONE);
    ESP_RETURN_ON_FALSE(
        vibe_cardputer_air_mouse_apply_settings(
            &runtime->input_profile.air_mouse),
        ESP_ERR_INVALID_ARG, TAG, "apply air mouse profile");
    ESP_RETURN_ON_ERROR(
        vibe_cardputer_volume_init(&config->volume),
        TAG, "initialize volume");
    ESP_RETURN_ON_ERROR(
        vibe_keyboard_init(
            config->keyboard_callback,
            config->keyboard_context),
        TAG, "initialize keyboard");
    ESP_RETURN_ON_ERROR(
        init_opt_timers(runtime),
        TAG, "initialize Opt gesture");

    runtime->initialized = true;
    return ESP_OK;
}

esp_err_t vibe_cardputer_runtime_start_messages(
    vibe_cardputer_runtime_t *runtime)
{
    ESP_RETURN_ON_FALSE(
        runtime && runtime->initialized,
        ESP_ERR_INVALID_STATE, TAG, "runtime not initialized");
    if (runtime->messages_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        vibe_cardputer_messages_init(&runtime->config.messages),
        TAG, "initialize message center");
    ESP_RETURN_ON_ERROR(
        vibe_cardputer_messages_start(),
        TAG, "start message center");
    runtime->messages_started = true;
    return ESP_OK;
}

vibe_cardputer_key_result_t vibe_cardputer_runtime_handle_feature_key(
    vibe_cardputer_runtime_t *runtime,
    const vibe_key_event_t *event)
{
    if (!runtime || !runtime->initialized || !event) {
        return VIBE_CARDPUTER_KEY_NOT_HANDLED;
    }
    if (vibe_cardputer_volume_handle_key(event) ||
        vibe_cardputer_messages_handle_key(event)) {
        return VIBE_CARDPUTER_KEY_CONSUMED_RELEASE_KEYBOARD;
    }
    if (vibe_cardputer_air_mouse_handle_key(event)) {
        return VIBE_CARDPUTER_KEY_CONSUMED;
    }
    return VIBE_CARDPUTER_KEY_NOT_HANDLED;
}

esp_err_t vibe_cardputer_runtime_apply_profile(
    vibe_cardputer_runtime_t *runtime,
    const vibe_card_input_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(
        runtime && runtime->initialized && profile &&
            vibe_card_input_profile_valid(profile),
        ESP_ERR_INVALID_ARG, TAG, "invalid input profile");
    ESP_RETURN_ON_FALSE(
        vibe_cardputer_air_mouse_apply_settings(&profile->air_mouse),
        ESP_ERR_INVALID_ARG, TAG, "apply air mouse settings");
    ESP_RETURN_ON_ERROR(
        vibe_card_input_profile_save(profile),
        TAG, "save input profile");
    lock_profile(runtime);
    runtime->input_profile = *profile;
    atomic_store(
        &runtime->profile_revision, profile->revision);
    atomic_store(
        &runtime->opt_tap_route, profile->opt_tap);
    atomic_store(
        &runtime->opt_double_route, profile->opt_double);
    atomic_store(
        &runtime->opt_hold_route, profile->opt_hold);
    unlock_profile(runtime);
    ESP_LOGI(
        TAG, "input profile applied revision=%lu",
        (unsigned long)profile->revision);
    return ESP_OK;
}

void vibe_cardputer_runtime_tick(
    vibe_cardputer_runtime_t *runtime,
    int64_t now_ms)
{
    if (runtime && runtime->initialized &&
        vibe_cardputer_air_mouse_enabled()) {
        vibe_cardputer_air_mouse_poll(now_ms);
    }
}

void vibe_cardputer_runtime_stop_interactive(
    vibe_cardputer_runtime_t *runtime)
{
    if (runtime && runtime->initialized) {
        vibe_cardputer_air_mouse_stop();
    }
}

void vibe_cardputer_runtime_release_display_resources(
    vibe_cardputer_runtime_t *runtime)
{
    if (runtime && runtime->messages_started) {
        vibe_cardputer_messages_release_display_resources();
    }
}

void vibe_cardputer_runtime_opt_press(
    vibe_cardputer_runtime_t *runtime)
{
    if (!runtime || !runtime->initialized ||
        atomic_exchange(&runtime->opt_down, true)) {
        return;
    }
    atomic_store(&runtime->opt_chord, false);
    atomic_store(&runtime->opt_button_committed, false);
    if (runtime->config.activity) {
        runtime->config.activity(runtime->config.context);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(
            opt_timer(runtime->opt_long_timer),
            runtime->config.opt_long_press_ms * 1000ULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(
            opt_timer(runtime->opt_confirm_timer),
            runtime->config.opt_confirm_hold_ms * 1000ULL));
}

void vibe_cardputer_runtime_opt_release(
    vibe_cardputer_runtime_t *runtime)
{
    if (!runtime || !runtime->initialized ||
        !atomic_exchange(&runtime->opt_down, false)) {
        return;
    }
    cancel_opt_hold_timers(runtime);
    if (atomic_exchange(&runtime->opt_chord, false)) {
        atomic_store(
            &runtime->opt_button_committed, false);
        return;
    }
    if (atomic_exchange(
            &runtime->opt_button_committed, false)) {
        (void)queue_opt_command(
            runtime,
            vibe_input_router_command(
                VIBE_INPUT_SIGNAL_CARD_OPT_HOLD_STOP));
        return;
    }

    if (runtime->config.front_down) {
        runtime->config.front_down(runtime->config.context);
    }
    if (runtime->config.front_up) {
        runtime->config.front_up(runtime->config.context);
    }
    const int clicks =
        atomic_fetch_add(
            &runtime->opt_pending_clicks, 1) + 1;
    if (clicks >= 2) {
        atomic_store(&runtime->opt_pending_clicks, 0);
        stop_opt_timer(runtime->opt_click_timer);
        (void)queue_opt_command(
            runtime,
            vibe_input_router_command(
                VIBE_INPUT_SIGNAL_CARD_OPT_DOUBLE));
    } else {
        stop_opt_timer(runtime->opt_click_timer);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_once(
                opt_timer(runtime->opt_click_timer),
                runtime->config.opt_click_window_ms * 1000ULL));
    }
}

bool vibe_cardputer_runtime_opt_mark_chord(
    vibe_cardputer_runtime_t *runtime)
{
    if (!runtime || !runtime->initialized ||
        !atomic_load(&runtime->opt_down)) {
        return true;
    }
    if (atomic_load(&runtime->opt_button_committed)) {
        return false;
    }
    atomic_store(&runtime->opt_chord, true);
    cancel_opt_hold_timers(runtime);
    return true;
}

bool vibe_cardputer_runtime_opt_chord_active(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->initialized &&
           atomic_load(&runtime->opt_chord);
}

bool vibe_cardputer_runtime_opt_busy(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->initialized &&
           (atomic_load(&runtime->opt_down) ||
            atomic_load(&runtime->opt_button_committed) ||
            atomic_load(&runtime->opt_pending_clicks) > 0 ||
            atomic_load(&runtime->opt_actions_pending) > 0);
}

void vibe_cardputer_runtime_opt_action_complete(
    vibe_cardputer_runtime_t *runtime)
{
    if (!runtime) return;
    unsigned pending =
        atomic_load(&runtime->opt_actions_pending);
    if (pending > 0) {
        atomic_fetch_sub(
            &runtime->opt_actions_pending, 1);
    }
}

vibe_card_input_route_t
vibe_cardputer_runtime_opt_active_hold_route(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->initialized
               ? (vibe_card_input_route_t)atomic_load(
                     &runtime->opt_active_hold_route)
               : VIBE_CARD_ROUTE_NONE;
}

bool vibe_cardputer_runtime_messages_busy(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->messages_started &&
           vibe_cardputer_messages_busy();
}

bool vibe_cardputer_runtime_air_mouse_enabled(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->initialized &&
           vibe_cardputer_air_mouse_enabled();
}

uint32_t vibe_cardputer_runtime_profile_revision(
    const vibe_cardputer_runtime_t *runtime)
{
    return runtime && runtime->initialized
               ? atomic_load(&runtime->profile_revision)
               : 0;
}

vibe_card_input_route_t vibe_cardputer_runtime_route(
    const vibe_cardputer_runtime_t *runtime,
    vibe_cardputer_opt_gesture_t opt_gesture)
{
    if (!runtime || !runtime->initialized) {
        return VIBE_CARD_ROUTE_NONE;
    }
    switch (opt_gesture) {
    case VIBE_CARDPUTER_OPT_TAP:
        return (vibe_card_input_route_t)atomic_load(
            &runtime->opt_tap_route);
    case VIBE_CARDPUTER_OPT_DOUBLE:
        return (vibe_card_input_route_t)atomic_load(
            &runtime->opt_double_route);
    case VIBE_CARDPUTER_OPT_HOLD:
        return (vibe_card_input_route_t)atomic_load(
            &runtime->opt_hold_route);
    default:
        return VIBE_CARD_ROUTE_NONE;
    }
}

void vibe_cardputer_runtime_snapshot(
    const vibe_cardputer_runtime_t *runtime,
    vibe_cardputer_runtime_snapshot_t *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!runtime || !runtime->initialized) return;
    lock_profile(runtime);
    snapshot->input_profile = runtime->input_profile;
    unlock_profile(runtime);
    snapshot->air_mouse_enabled =
        vibe_cardputer_runtime_air_mouse_enabled(runtime);
    snapshot->messages_busy =
        vibe_cardputer_runtime_messages_busy(runtime);
    snapshot->message_storage_ready =
        runtime->messages_started &&
        vibe_cardputer_messages_storage_ready();
}
