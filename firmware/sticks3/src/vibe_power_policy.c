#include "vibe_power_policy.h"

vibe_power_policy_display_state_t
vibe_power_policy_display_state(const vibe_power_policy_input_t *input)
{
    if (!input || input->active_work) {
        return VIBE_POWER_POLICY_DISPLAY_ACTIVE;
    }
    if (input->false_wake_sleep_due) {
        return VIBE_POWER_POLICY_DISPLAY_OFF;
    }
    const int64_t idle_ms = input->now_ms - input->last_activity_ms;
    if (idle_ms >= input->off_after_ms) {
        return VIBE_POWER_POLICY_DISPLAY_OFF;
    }
    if (idle_ms >= input->dim_after_ms) {
        return VIBE_POWER_POLICY_DISPLAY_DIMMED;
    }
    return VIBE_POWER_POLICY_DISPLAY_ACTIVE;
}

bool vibe_power_policy_should_attempt_deep_sleep(bool active_work,
                                                 bool ota_in_progress,
                                                 bool motion_start_pending,
                                                 int64_t now_ms,
                                                 int64_t last_activity_ms,
                                                 int64_t deep_sleep_after_ms)
{
    return !active_work &&
           !ota_in_progress &&
           !motion_start_pending &&
           last_activity_ms > 0 &&
           now_ms - last_activity_ms >= deep_sleep_after_ms;
}
