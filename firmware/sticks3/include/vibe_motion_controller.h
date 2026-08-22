#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t calibration_timeout_ms;
    int64_t wake_confirm_ms;
    int64_t network_timeout_ms;
} vibe_motion_controller_config_t;

typedef struct {
    void *context;
} vibe_motion_controller_dependencies_t;

typedef enum {
    VIBE_MOTION_COMMAND_RESET,
    VIBE_MOTION_COMMAND_SET_ARM_PROMPT,
    VIBE_MOTION_COMMAND_BEGIN_CALIBRATION,
    VIBE_MOTION_COMMAND_CALIBRATION_COMPLETE,
    VIBE_MOTION_COMMAND_RESTORE_CALIBRATION,
    VIBE_MOTION_COMMAND_SET_LIFT_READY,
    VIBE_MOTION_COMMAND_SET_ARMED,
    VIBE_MOTION_COMMAND_SET_START_PENDING,
    VIBE_MOTION_COMMAND_BEGIN_WAKE_CONFIRM,
    VIBE_MOTION_COMMAND_CANCEL_WAKE_CONFIRM,
    VIBE_MOTION_COMMAND_BEGIN_NETWORK_WAIT,
    VIBE_MOTION_COMMAND_CANCEL_NETWORK_WAIT,
    VIBE_MOTION_COMMAND_SET_FALSE_WAKE_UNTIL,
    VIBE_MOTION_COMMAND_CLEAR_FALSE_WAKE,
    VIBE_MOTION_COMMAND_CANCEL_PENDING,
} vibe_motion_controller_command_type_t;

typedef struct {
    vibe_motion_controller_command_type_t type;
    union {
        bool flag;
        int64_t now_ms;
        int64_t deadline_ms;
        struct {
            int64_t now_ms;
            bool had_previous;
        } calibration;
    } data;
} vibe_motion_controller_command_t;

typedef struct {
    bool calibrating;
    int64_t calibration_deadline_ms;
    bool calibration_had_previous;
    bool lift_armed;
    bool arm_prompt_visible;
    bool start_pending;
    bool wake_confirm_pending;
    int64_t wake_confirm_deadline_ms;
    bool wake_network_pending;
    int64_t wake_network_deadline_ms;
    int64_t false_wake_sleep_deadline_ms;
    bool calibration_timeout_due;
    bool wake_confirm_due;
    bool network_timeout_due;
    bool false_wake_due;
} vibe_motion_controller_snapshot_t;

typedef struct {
    vibe_motion_controller_config_t config;
    vibe_motion_controller_dependencies_t dependencies;
    vibe_motion_controller_snapshot_t state;
} vibe_motion_controller_t;

void vibe_motion_controller_init(
    vibe_motion_controller_t *controller,
    const vibe_motion_controller_config_t *config,
    const vibe_motion_controller_dependencies_t *dependencies);
bool vibe_motion_controller_handle(
    vibe_motion_controller_t *controller,
    const vibe_motion_controller_command_t *command);
void vibe_motion_controller_tick(
    vibe_motion_controller_t *controller,
    int64_t now_ms);
void vibe_motion_controller_snapshot(
    const vibe_motion_controller_t *controller,
    vibe_motion_controller_snapshot_t *snapshot);
