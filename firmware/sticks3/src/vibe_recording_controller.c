#include "vibe_recording_controller.h"

#include <string.h>

static recording_intent_t sanitize_intent(
    const vibe_recording_controller_t *controller,
    recording_intent_t intent)
{
    if (!controller->config.cyber_intents &&
        intent != RECORDING_INTENT_DICTATION) {
        return RECORDING_INTENT_DICTATION;
    }
    if (intent < RECORDING_INTENT_DICTATION ||
        intent > RECORDING_INTENT_CYBER_ALMANAC) {
        return RECORDING_INTENT_DICTATION;
    }
    return intent;
}

void vibe_recording_controller_init(
    vibe_recording_controller_t *controller,
    const vibe_recording_controller_config_t *config,
    const vibe_recording_controller_dependencies_t *dependencies)
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
    controller->trigger_mode = RECORDING_TRIGGER_PUSH_TO_TALK;
    controller->intent = RECORDING_INTENT_DICTATION;
    atomic_init(&controller->session_active, false);
    atomic_init(&controller->finalize_active, false);
}

bool vibe_recording_controller_handle(
    vibe_recording_controller_t *controller,
    const vibe_recording_command_t *command)
{
    if (!controller || !command) {
        return false;
    }
    switch (command->type) {
    case VIBE_RECORDING_COMMAND_SET_TRIGGER:
        if (command->data.trigger != RECORDING_TRIGGER_PUSH_TO_TALK &&
            command->data.trigger != RECORDING_TRIGGER_LIFT_TO_TALK) {
            return false;
        }
        controller->trigger_mode = command->data.trigger;
        return true;
    case VIBE_RECORDING_COMMAND_SET_INTENT:
        controller->intent =
            sanitize_intent(controller, command->data.intent);
        return true;
    case VIBE_RECORDING_COMMAND_CYCLE_INTENT:
        if (!controller->config.cyber_intents) {
            controller->intent = RECORDING_INTENT_DICTATION;
        } else if (controller->intent == RECORDING_INTENT_DICTATION) {
            controller->intent = RECORDING_INTENT_CYBER_FORTUNE;
        } else if (controller->intent == RECORDING_INTENT_CYBER_FORTUNE) {
            controller->intent = RECORDING_INTENT_CYBER_ALMANAC;
        } else {
            controller->intent = RECORDING_INTENT_DICTATION;
        }
        return true;
    case VIBE_RECORDING_COMMAND_BEGIN_SESSION:
        if (!command->data.begin_session.session_id ||
            command->data.begin_session.session_id[0] == '\0') {
            return false;
        }
        if (strlen(command->data.begin_session.session_id) >=
            sizeof(controller->session_id)) {
            return false;
        }
        strcpy(controller->session_id,
               command->data.begin_session.session_id);
        controller->chunk_id = 0;
        controller->uploaded_bytes = 0;
        controller->upload_abort_requested = false;
        controller->local_capture = true;
        controller->bridge_stop_required =
            command->data.begin_session.notify_bridge;
        atomic_store(&controller->session_active, true);
        return true;
    case VIBE_RECORDING_COMMAND_SET_SESSION_ID:
        if (!command->data.session_id ||
            command->data.session_id[0] == '\0' ||
            strlen(command->data.session_id) >=
                sizeof(controller->session_id)) {
            return false;
        }
        strcpy(controller->session_id, command->data.session_id);
        return true;
    case VIBE_RECORDING_COMMAND_SET_CAPTURE_MODE:
        controller->local_capture = command->data.flag;
        return true;
    case VIBE_RECORDING_COMMAND_NOTE_CHUNK:
        controller->chunk_id++;
        controller->uploaded_bytes += command->data.chunk_bytes;
        return true;
    case VIBE_RECORDING_COMMAND_SET_UPLOAD_TOTALS:
        controller->chunk_id = command->data.upload_totals.chunk_count;
        controller->uploaded_bytes =
            command->data.upload_totals.uploaded_bytes;
        return true;
    case VIBE_RECORDING_COMMAND_RESET_SESSION:
        controller->session_id[0] = '\0';
        controller->local_capture = false;
        controller->bridge_stop_required = false;
        controller->upload_abort_requested = false;
        controller->tap_active = false;
        controller->motion_active = false;
        atomic_store(&controller->session_active, false);
        return true;
    case VIBE_RECORDING_COMMAND_SET_TAP_ACTIVE:
        controller->tap_active = command->data.flag;
        return true;
    case VIBE_RECORDING_COMMAND_SET_MOTION_ACTIVE:
        controller->motion_active = command->data.flag;
        return true;
    case VIBE_RECORDING_COMMAND_SET_SESSION_ACTIVE:
        atomic_store(&controller->session_active, command->data.flag);
        return true;
    case VIBE_RECORDING_COMMAND_SET_FINALIZE_ACTIVE:
        atomic_store(&controller->finalize_active, command->data.flag);
        return true;
    case VIBE_RECORDING_COMMAND_REQUEST_UPLOAD_ABORT:
        controller->upload_abort_requested = true;
        return true;
    default:
        return false;
    }
}

void vibe_recording_controller_tick(
    vibe_recording_controller_t *controller,
    int64_t now_ms)
{
    if (!controller) {
        return;
    }
    if (vibe_recording_followup_present(&controller->followup) &&
        now_ms > controller->followup.deadline_ms) {
        vibe_recording_followup_clear(&controller->followup);
    }
}

void vibe_recording_controller_snapshot(
    const vibe_recording_controller_t *controller,
    vibe_recording_snapshot_t *snapshot)
{
    if (!controller || !snapshot) {
        return;
    }
    snapshot->trigger_mode = controller->trigger_mode;
    snapshot->intent = controller->intent;
    memcpy(snapshot->session_id, controller->session_id,
           sizeof(snapshot->session_id));
    snapshot->chunk_id = controller->chunk_id;
    snapshot->uploaded_bytes = controller->uploaded_bytes;
    snapshot->local_capture = controller->local_capture;
    snapshot->bridge_stop_required = controller->bridge_stop_required;
    snapshot->upload_abort_requested =
        controller->upload_abort_requested;
    snapshot->tap_active = controller->tap_active;
    snapshot->motion_active = controller->motion_active;
    snapshot->session_active =
        atomic_load(&controller->session_active);
    snapshot->finalize_active =
        atomic_load(&controller->finalize_active);
}

bool vibe_recording_controller_intent_supported(
    const vibe_recording_controller_t *controller,
    recording_intent_t intent)
{
    return controller &&
           (intent == RECORDING_INTENT_DICTATION ||
            controller->config.cyber_intents);
}

const char *vibe_recording_controller_trigger_label(
    const vibe_recording_controller_t *controller,
    bool calibrating)
{
    if (!controller) {
        return "PTT";
    }
    if (controller->trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK &&
        calibrating) {
        return "CAL";
    }
    return controller->trigger_mode == RECORDING_TRIGGER_LIFT_TO_TALK
               ? "LIFT"
               : "PTT";
}

const char *vibe_recording_controller_intent_label(
    const vibe_recording_controller_t *controller)
{
    if (!controller ||
        !vibe_recording_controller_intent_supported(
            controller, controller->intent)) {
        return "DICT";
    }
    if (controller->intent == RECORDING_INTENT_CYBER_FORTUNE) {
        return "FORT";
    }
    if (controller->intent == RECORDING_INTENT_CYBER_ALMANAC) {
        return "ALM";
    }
    return "DICT";
}

const char *vibe_recording_controller_intent_key(
    const vibe_recording_controller_t *controller)
{
    if (!controller ||
        !vibe_recording_controller_intent_supported(
            controller, controller->intent)) {
        return "dictation";
    }
    if (controller->intent == RECORDING_INTENT_CYBER_FORTUNE) {
        return "cyber_fortune";
    }
    if (controller->intent == RECORDING_INTENT_CYBER_ALMANAC) {
        return "cyber_almanac";
    }
    return "dictation";
}

bool vibe_recording_controller_is_cyber(
    const vibe_recording_controller_t *controller)
{
    return controller &&
           vibe_recording_controller_intent_supported(
               controller, controller->intent) &&
           controller->intent != RECORDING_INTENT_DICTATION;
}
