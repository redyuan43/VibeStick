#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_RECORDING_POLICY_SESSION_ID_LEN 40

typedef struct {
    char session_id[VIBE_RECORDING_POLICY_SESSION_ID_LEN];
    int64_t deadline_ms;
} vibe_recording_followup_window_t;

void vibe_recording_followup_clear(vibe_recording_followup_window_t *window);
bool vibe_recording_followup_arm(vibe_recording_followup_window_t *window,
                                 const char *session_id,
                                 int64_t now_ms,
                                 int64_t grace_ms);
bool vibe_recording_followup_present(const vibe_recording_followup_window_t *window);
bool vibe_recording_followup_consume(vibe_recording_followup_window_t *window,
                                     bool push_to_talk,
                                     int64_t now_ms);
