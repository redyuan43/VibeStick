#include "vibe_motion_controller.h"

#include <string.h>

static void clear_pending(vibe_motion_controller_snapshot_t *state)
{
    state->lift_armed = false;
    state->start_pending = false;
    state->wake_confirm_pending = false;
    state->wake_confirm_deadline_ms = 0;
    state->wake_network_pending = false;
    state->wake_network_deadline_ms = 0;
}

void vibe_motion_controller_init(
    vibe_motion_controller_t *controller,
    const vibe_motion_controller_config_t *config,
    const vibe_motion_controller_dependencies_t *dependencies)
{
    if (!controller) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    if (config) {
        controller->config = *config;
    }
    if (dependencies) {
        controller->dependencies = *dependencies;
    }
}

bool vibe_motion_controller_handle(
    vibe_motion_controller_t *controller,
    const vibe_motion_controller_command_t *command)
{
    if (!controller || !command) {
        return false;
    }
    vibe_motion_controller_snapshot_t *state = &controller->state;
    switch (command->type) {
    case VIBE_MOTION_COMMAND_RESET:
        memset(state, 0, sizeof(*state));
        return true;
    case VIBE_MOTION_COMMAND_SET_ARM_PROMPT:
        state->arm_prompt_visible = command->data.flag;
        return true;
    case VIBE_MOTION_COMMAND_BEGIN_CALIBRATION:
        state->calibrating = true;
        state->calibration_had_previous =
            command->data.calibration.had_previous;
        state->calibration_deadline_ms =
            command->data.calibration.now_ms +
            controller->config.calibration_timeout_ms;
        state->false_wake_sleep_deadline_ms = 0;
        clear_pending(state);
        return true;
    case VIBE_MOTION_COMMAND_CALIBRATION_COMPLETE:
        state->calibrating = false;
        state->calibration_deadline_ms = 0;
        state->calibration_had_previous = false;
        state->lift_armed = true;
        state->start_pending = false;
        return true;
    case VIBE_MOTION_COMMAND_RESTORE_CALIBRATION:
        state->calibrating = false;
        state->calibration_deadline_ms = 0;
        state->calibration_had_previous = false;
        state->lift_armed = false;
        state->start_pending = false;
        return true;
    case VIBE_MOTION_COMMAND_SET_LIFT_READY:
        state->calibrating = false;
        state->calibration_deadline_ms = 0;
        state->calibration_had_previous = false;
        state->lift_armed = false;
        state->start_pending = false;
        return true;
    case VIBE_MOTION_COMMAND_SET_ARMED:
        state->lift_armed = command->data.flag;
        return true;
    case VIBE_MOTION_COMMAND_SET_START_PENDING:
        state->start_pending = command->data.flag;
        return true;
    case VIBE_MOTION_COMMAND_BEGIN_WAKE_CONFIRM:
        state->wake_confirm_pending = true;
        state->wake_confirm_deadline_ms =
            command->data.now_ms + controller->config.wake_confirm_ms;
        state->lift_armed = false;
        state->start_pending = false;
        state->wake_network_pending = false;
        state->wake_network_deadline_ms = 0;
        return true;
    case VIBE_MOTION_COMMAND_CANCEL_WAKE_CONFIRM:
        state->wake_confirm_pending = false;
        state->wake_confirm_deadline_ms = 0;
        return true;
    case VIBE_MOTION_COMMAND_BEGIN_NETWORK_WAIT:
        state->wake_network_pending = true;
        state->wake_network_deadline_ms =
            command->data.now_ms + controller->config.network_timeout_ms;
        state->start_pending = true;
        state->lift_armed = false;
        return true;
    case VIBE_MOTION_COMMAND_CANCEL_NETWORK_WAIT:
        state->wake_network_pending = false;
        state->wake_network_deadline_ms = 0;
        return true;
    case VIBE_MOTION_COMMAND_SET_FALSE_WAKE_UNTIL:
        state->false_wake_sleep_deadline_ms =
            command->data.deadline_ms;
        return true;
    case VIBE_MOTION_COMMAND_CLEAR_FALSE_WAKE:
        state->false_wake_sleep_deadline_ms = 0;
        return true;
    case VIBE_MOTION_COMMAND_CANCEL_PENDING:
        clear_pending(state);
        return true;
    default:
        return false;
    }
}

void vibe_motion_controller_tick(
    vibe_motion_controller_t *controller,
    int64_t now_ms)
{
    if (!controller) {
        return;
    }
    vibe_motion_controller_snapshot_t *state = &controller->state;
    state->calibration_timeout_due =
        state->calibrating &&
        state->calibration_deadline_ms > 0 &&
        now_ms >= state->calibration_deadline_ms;
    state->wake_confirm_due =
        state->wake_confirm_pending &&
        state->wake_confirm_deadline_ms > 0 &&
        now_ms >= state->wake_confirm_deadline_ms;
    state->network_timeout_due =
        state->wake_network_pending &&
        state->wake_network_deadline_ms > 0 &&
        now_ms >= state->wake_network_deadline_ms;
    state->false_wake_due =
        state->false_wake_sleep_deadline_ms > 0 &&
        now_ms >= state->false_wake_sleep_deadline_ms;
}

void vibe_motion_controller_snapshot(
    const vibe_motion_controller_t *controller,
    vibe_motion_controller_snapshot_t *snapshot)
{
    if (!controller || !snapshot) {
        return;
    }
    *snapshot = controller->state;
}
