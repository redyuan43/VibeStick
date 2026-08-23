#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "vibe_recording_policy.h"

typedef enum {
    RECORDING_TRIGGER_PUSH_TO_TALK,
    RECORDING_TRIGGER_LIFT_TO_TALK,
} recording_trigger_mode_t;

typedef enum {
    RECORDING_INTENT_DICTATION,
    RECORDING_INTENT_CYBER_FORTUNE,
    RECORDING_INTENT_CYBER_ALMANAC,
} recording_intent_t;

typedef struct {
    bool cyber_intents;
} vibe_recording_controller_config_t;

typedef struct {
    void *context;
} vibe_recording_controller_dependencies_t;

typedef enum {
    VIBE_RECORDING_COMMAND_SET_TRIGGER,
    VIBE_RECORDING_COMMAND_SET_INTENT,
    VIBE_RECORDING_COMMAND_CYCLE_INTENT,
    VIBE_RECORDING_COMMAND_BEGIN_SESSION,
    VIBE_RECORDING_COMMAND_SET_SESSION_ID,
    VIBE_RECORDING_COMMAND_SET_CAPTURE_MODE,
    VIBE_RECORDING_COMMAND_NOTE_CHUNK,
    VIBE_RECORDING_COMMAND_SET_UPLOAD_TOTALS,
    VIBE_RECORDING_COMMAND_RESET_SESSION,
    VIBE_RECORDING_COMMAND_SET_TAP_ACTIVE,
    VIBE_RECORDING_COMMAND_SET_MOTION_ACTIVE,
    VIBE_RECORDING_COMMAND_SET_SESSION_ACTIVE,
    VIBE_RECORDING_COMMAND_SET_FINALIZE_ACTIVE,
    VIBE_RECORDING_COMMAND_REQUEST_UPLOAD_ABORT,
} vibe_recording_command_type_t;

typedef struct {
    vibe_recording_command_type_t type;
    union {
        recording_trigger_mode_t trigger;
        recording_intent_t intent;
        struct {
            const char *session_id;
            bool notify_bridge;
        } begin_session;
        const char *session_id;
        bool flag;
        uint32_t chunk_bytes;
        struct {
            uint32_t chunk_count;
            uint32_t uploaded_bytes;
        } upload_totals;
    } data;
} vibe_recording_command_t;

typedef struct {
    recording_trigger_mode_t trigger_mode;
    recording_intent_t intent;
    char session_id[VIBE_RECORDING_POLICY_SESSION_ID_LEN];
    uint32_t chunk_id;
    uint32_t uploaded_bytes;
    bool local_capture;
    bool bridge_stop_required;
    bool upload_abort_requested;
    bool tap_active;
    bool motion_active;
    bool session_active;
    bool finalize_active;
} vibe_recording_snapshot_t;

typedef struct {
    vibe_recording_controller_config_t config;
    vibe_recording_controller_dependencies_t dependencies;
    recording_trigger_mode_t trigger_mode;
    recording_intent_t intent;
    char session_id[VIBE_RECORDING_POLICY_SESSION_ID_LEN];
    uint32_t chunk_id;
    uint32_t uploaded_bytes;
    bool local_capture;
    bool bridge_stop_required;
    bool upload_abort_requested;
    bool tap_active;
    bool motion_active;
    atomic_bool session_active;
    atomic_bool finalize_active;
    vibe_recording_followup_window_t followup;
} vibe_recording_controller_t;

void vibe_recording_controller_init(
    vibe_recording_controller_t *controller,
    const vibe_recording_controller_config_t *config,
    const vibe_recording_controller_dependencies_t *dependencies);
bool vibe_recording_controller_handle(
    vibe_recording_controller_t *controller,
    const vibe_recording_command_t *command);
void vibe_recording_controller_tick(
    vibe_recording_controller_t *controller,
    int64_t now_ms);
void vibe_recording_controller_snapshot(
    const vibe_recording_controller_t *controller,
    vibe_recording_snapshot_t *snapshot);

bool vibe_recording_controller_intent_supported(
    const vibe_recording_controller_t *controller,
    recording_intent_t intent);
const char *vibe_recording_controller_trigger_label(
    const vibe_recording_controller_t *controller,
    bool calibrating);
const char *vibe_recording_controller_intent_label(
    const vibe_recording_controller_t *controller);
const char *vibe_recording_controller_intent_key(
    const vibe_recording_controller_t *controller);
bool vibe_recording_controller_is_cyber(
    const vibe_recording_controller_t *controller);
