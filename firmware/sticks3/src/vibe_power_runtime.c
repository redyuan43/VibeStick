#include "vibe_power_runtime.h"

#include <string.h>

#include "vibe_power_policy.h"

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

static int median_sample(
    const vibe_power_runtime_snapshot_t *state)
{
    int samples[VIBE_POWER_RUNTIME_MAX_SAMPLES] = {0};
    for (size_t index = 0;
         index < state->battery_sample_count; ++index) {
        samples[index] = state->battery_samples[index];
    }
    for (size_t index = 1;
         index < state->battery_sample_count; ++index) {
        const int value = samples[index];
        size_t position = index;
        while (position > 0 &&
               samples[position - 1] > value) {
            samples[position] = samples[position - 1];
            position--;
        }
        samples[position] = value;
    }
    return samples[state->battery_sample_count / 2];
}

static void record_battery(
    vibe_power_runtime_t *runtime,
    const vibe_power_runtime_command_t *command)
{
    vibe_power_runtime_snapshot_t *state = &runtime->state;
    state->battery_raw_level =
        clamp_percent(command->data.battery.raw_level);
    state->battery_samples[state->battery_sample_index] =
        state->battery_raw_level;
    state->battery_sample_index =
        (state->battery_sample_index + 1) %
        VIBE_POWER_RUNTIME_MAX_SAMPLES;
    if (state->battery_sample_count <
        VIBE_POWER_RUNTIME_MAX_SAMPLES) {
        state->battery_sample_count++;
    }

    int target_level = median_sample(state);
    if (state->battery_full_latched) {
        target_level = 100;
    }
    if (!state->battery_display_valid) {
        if (command->data.battery.woke_from_deep_sleep &&
            command->data.battery.retained_valid &&
            command->data.battery.retained_level >= 0 &&
            command->data.battery.retained_level <= 100) {
            state->battery_display_level =
                command->data.battery.retained_level;
        } else {
            state->battery_display_level = target_level;
        }
        state->battery_display_valid = true;
    }

    const int64_t now_ms = command->data.battery.now_ms;
    const bool hold_drop =
        (state->external_power_removed_ms != 0 &&
         now_ms - state->external_power_removed_ms <
             runtime->config.usb_unplug_hold_ms) ||
        (state->deep_sleep_wake_ms != 0 &&
         now_ms - state->deep_sleep_wake_ms <
             runtime->config.wake_stabilize_ms);
    if (hold_drop &&
        target_level < state->battery_display_level) {
        target_level = state->battery_display_level;
    }

    if (target_level > state->battery_display_level) {
        state->battery_display_level++;
    } else if (target_level < state->battery_display_level) {
        state->battery_display_level--;
    }
}

void vibe_power_runtime_init(
    vibe_power_runtime_t *runtime,
    const vibe_power_runtime_config_t *config,
    const vibe_power_runtime_dependencies_t *dependencies,
    uint8_t initial_backlight)
{
    if (!runtime) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    if (config) {
        runtime->config = *config;
    }
    if (dependencies) {
        runtime->dependencies = *dependencies;
    }
    runtime->state.battery_raw_level = -1;
    runtime->state.battery_voltage_mv = -1;
    runtime->state.battery_display_level = -1;
    runtime->state.current_backlight = initial_backlight;
    runtime->state.display_state = DISPLAY_POWER_ACTIVE;
}

bool vibe_power_runtime_handle(
    vibe_power_runtime_t *runtime,
    const vibe_power_runtime_command_t *command)
{
    if (!runtime || !command) {
        return false;
    }
    switch (command->type) {
    case VIBE_POWER_COMMAND_ACTIVITY:
        runtime->state.last_activity_ms = command->data.time_ms;
        runtime->state.display_state = DISPLAY_POWER_ACTIVE;
        return true;
    case VIBE_POWER_COMMAND_SET_WAKE_TIME:
        runtime->state.deep_sleep_wake_ms = command->data.time_ms;
        return true;
    case VIBE_POWER_COMMAND_SET_EXTERNAL_POWER_REMOVED_TIME:
        runtime->state.external_power_removed_ms =
            command->data.time_ms;
        return true;
    case VIBE_POWER_COMMAND_SET_FULL_LATCH:
        runtime->state.battery_full_latched = command->data.flag;
        return true;
    case VIBE_POWER_COMMAND_RECORD_BATTERY:
        record_battery(runtime, command);
        return true;
    case VIBE_POWER_COMMAND_SET_VOLTAGE:
        runtime->state.battery_voltage_mv = command->data.value;
        return true;
    case VIBE_POWER_COMMAND_SET_CHARGING:
        runtime->state.charging = command->data.flag;
        return true;
    case VIBE_POWER_COMMAND_SET_USB_POWERED:
        runtime->state.usb_powered = command->data.flag;
        return true;
    case VIBE_POWER_COMMAND_SET_BACKLIGHT:
        runtime->state.current_backlight =
            command->data.brightness;
        return true;
    default:
        return false;
    }
}

void vibe_power_runtime_tick(vibe_power_runtime_t *runtime,
                             int64_t now_ms)
{
    if (!runtime) {
        return;
    }
    if (runtime->state.last_activity_ms == 0) {
        runtime->state.last_activity_ms = now_ms;
    }
    const vibe_power_policy_input_t input = {
        .active_work =
            runtime->dependencies.active_work &&
            runtime->dependencies.active_work(
                runtime->dependencies.context),
        .false_wake_sleep_due =
            runtime->dependencies.false_wake_due &&
            runtime->dependencies.false_wake_due(
                runtime->dependencies.context),
        .now_ms = now_ms,
        .last_activity_ms = runtime->state.last_activity_ms,
        .dim_after_ms = runtime->config.dim_after_ms,
        .off_after_ms = runtime->config.off_after_ms,
    };
    runtime->state.display_state =
        (vibe_power_display_state_t)
            vibe_power_policy_display_state(&input);
}

void vibe_power_runtime_snapshot(
    const vibe_power_runtime_t *runtime,
    vibe_power_runtime_snapshot_t *snapshot)
{
    if (!runtime || !snapshot) {
        return;
    }
    *snapshot = runtime->state;
}
