#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VIBE_POWER_POLICY_DISPLAY_ACTIVE,
    VIBE_POWER_POLICY_DISPLAY_DIMMED,
    VIBE_POWER_POLICY_DISPLAY_OFF,
} vibe_power_policy_display_state_t;

typedef struct {
    bool active_work;
    bool false_wake_sleep_due;
    int64_t now_ms;
    int64_t last_activity_ms;
    int64_t dim_after_ms;
    int64_t off_after_ms;
} vibe_power_policy_input_t;

vibe_power_policy_display_state_t
vibe_power_policy_display_state(const vibe_power_policy_input_t *input);

bool vibe_power_policy_should_attempt_deep_sleep(bool active_work,
                                                 bool ota_in_progress,
                                                 bool motion_start_pending,
                                                 int64_t now_ms,
                                                 int64_t last_activity_ms,
                                                 int64_t deep_sleep_after_ms);
