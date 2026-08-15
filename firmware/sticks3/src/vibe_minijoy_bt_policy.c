#include "vibe_minijoy_bt_policy.h"

#include <stdio.h>
#include <string.h>

bool vibe_minijoy_bt_format_device_name(const uint8_t mac[6], char *out,
                                         size_t out_size)
{
    if (!mac || !out || out_size == 0) {
        return false;
    }
    int length = snprintf(out, out_size, "VibeStick-%02X%02X", mac[4],
                          mac[5]);
    return length > 0 && (size_t)length < out_size;
}

vibe_minijoy_power_state_t vibe_minijoy_bt_desired_power_state(
    bool active_work,
    bool usb_power_valid,
    bool usb_powered,
    int64_t now_ms,
    int64_t last_activity_ms,
    int64_t standby_after_ms,
    int64_t deep_sleep_after_ms)
{
    if (active_work || !usb_power_valid || usb_powered ||
        last_activity_ms <= 0 || now_ms < last_activity_ms) {
        return VIBE_MINIJOY_POWER_ACTIVE;
    }
    int64_t idle_ms = now_ms - last_activity_ms;
    if (idle_ms >= deep_sleep_after_ms) {
        return VIBE_MINIJOY_POWER_DEEP_SLEEP;
    }
    if (idle_ms >= standby_after_ms) {
        return VIBE_MINIJOY_POWER_STANDBY;
    }
    return VIBE_MINIJOY_POWER_ACTIVE;
}

bool vibe_minijoy_bt_should_attempt_automatic_sleep(
    bool active_work,
    bool usb_power_valid,
    bool usb_powered,
    int64_t now_ms,
    int64_t last_activity_ms,
    int64_t deep_sleep_after_ms)
{
    return vibe_minijoy_bt_desired_power_state(
               active_work, usb_power_valid, usb_powered, now_ms,
               last_activity_ms, deep_sleep_after_ms, deep_sleep_after_ms) ==
           VIBE_MINIJOY_POWER_DEEP_SLEEP;
}

vibe_minijoy_ptt_press_action_t vibe_minijoy_ptt_press_action(
    bool serial_control,
    bool capture_active,
    bool followup_available)
{
    if (capture_active) {
        return VIBE_MINIJOY_PTT_PRESS_NOOP;
    }
    if (!serial_control && followup_available) {
        return VIBE_MINIJOY_PTT_PRESS_FOLLOWUP;
    }
    return VIBE_MINIJOY_PTT_PRESS_START;
}

void vibe_minijoy_ptt_audio_clear(vibe_minijoy_ptt_audio_guard_t *guard)
{
    if (guard) {
        memset(guard, 0, sizeof(*guard));
    }
}

void vibe_minijoy_ptt_audio_begin(vibe_minijoy_ptt_audio_guard_t *guard,
                                  bool audio_connected,
                                  int64_t now_ms,
                                  int64_t grace_ms)
{
    if (!guard) {
        return;
    }
    guard->state = audio_connected ? VIBE_MINIJOY_PTT_AUDIO_CONNECTED
                                   : VIBE_MINIJOY_PTT_AUDIO_WAITING;
    guard->deadline_ms = audio_connected ? 0 : now_ms + grace_ms;
}

vibe_minijoy_ptt_audio_state_t vibe_minijoy_ptt_audio_update(
    vibe_minijoy_ptt_audio_guard_t *guard,
    bool ptt_active,
    bool audio_connected,
    int64_t now_ms,
    int64_t grace_ms)
{
    if (!guard) {
        return VIBE_MINIJOY_PTT_AUDIO_IDLE;
    }
    if (!ptt_active) {
        vibe_minijoy_ptt_audio_clear(guard);
        return guard->state;
    }
    if (audio_connected) {
        guard->state = VIBE_MINIJOY_PTT_AUDIO_CONNECTED;
        guard->deadline_ms = 0;
        return guard->state;
    }
    if (guard->state == VIBE_MINIJOY_PTT_AUDIO_CONNECTED ||
        guard->state == VIBE_MINIJOY_PTT_AUDIO_IDLE) {
        guard->state = VIBE_MINIJOY_PTT_AUDIO_WAITING;
        guard->deadline_ms = now_ms + grace_ms;
    }
    if (guard->state == VIBE_MINIJOY_PTT_AUDIO_WAITING &&
        now_ms >= guard->deadline_ms) {
        guard->state = VIBE_MINIJOY_PTT_AUDIO_FAILED;
        guard->deadline_ms = 0;
    }
    return guard->state;
}
