#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VIBE_MINIJOY_PTT_PRESS_NOOP,
    VIBE_MINIJOY_PTT_PRESS_START,
    VIBE_MINIJOY_PTT_PRESS_FOLLOWUP,
} vibe_minijoy_ptt_press_action_t;

typedef enum {
    VIBE_MINIJOY_PTT_AUDIO_IDLE,
    VIBE_MINIJOY_PTT_AUDIO_WAITING,
    VIBE_MINIJOY_PTT_AUDIO_CONNECTED,
    VIBE_MINIJOY_PTT_AUDIO_FAILED,
} vibe_minijoy_ptt_audio_state_t;

typedef struct {
    vibe_minijoy_ptt_audio_state_t state;
    int64_t deadline_ms;
} vibe_minijoy_ptt_audio_guard_t;

bool vibe_minijoy_bt_should_attempt_automatic_sleep(
    bool active_work,
    bool usb_power_valid,
    bool usb_powered,
    int64_t now_ms,
    int64_t last_activity_ms,
    int64_t deep_sleep_after_ms);

vibe_minijoy_ptt_press_action_t vibe_minijoy_ptt_press_action(
    bool serial_control,
    bool capture_active,
    bool followup_available);

void vibe_minijoy_ptt_audio_begin(vibe_minijoy_ptt_audio_guard_t *guard,
                                  bool audio_connected,
                                  int64_t now_ms,
                                  int64_t grace_ms);
void vibe_minijoy_ptt_audio_clear(vibe_minijoy_ptt_audio_guard_t *guard);
vibe_minijoy_ptt_audio_state_t vibe_minijoy_ptt_audio_update(
    vibe_minijoy_ptt_audio_guard_t *guard,
    bool ptt_active,
    bool audio_connected,
    int64_t now_ms,
    int64_t grace_ms);
