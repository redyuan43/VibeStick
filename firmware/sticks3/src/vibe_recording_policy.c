#include "vibe_recording_policy.h"

#include <string.h>

void vibe_recording_followup_clear(vibe_recording_followup_window_t *window)
{
    if (!window) {
        return;
    }
    memset(window, 0, sizeof(*window));
}

bool vibe_recording_followup_arm(vibe_recording_followup_window_t *window,
                                 const char *session_id,
                                 int64_t now_ms,
                                 int64_t grace_ms)
{
    if (!window || !session_id || session_id[0] == '\0' || grace_ms <= 0) {
        vibe_recording_followup_clear(window);
        return false;
    }
    size_t session_len = 0;
    while (session_len < sizeof(window->session_id) &&
           session_id[session_len] != '\0') {
        session_len++;
    }
    if (session_len == sizeof(window->session_id)) {
        vibe_recording_followup_clear(window);
        return false;
    }
    memcpy(window->session_id, session_id, session_len + 1);
    window->deadline_ms = now_ms + grace_ms;
    return true;
}

bool vibe_recording_followup_present(const vibe_recording_followup_window_t *window)
{
    return window && window->session_id[0] != '\0' && window->deadline_ms > 0;
}

bool vibe_recording_followup_consume(vibe_recording_followup_window_t *window,
                                     bool push_to_talk,
                                     int64_t now_ms)
{
    if (!push_to_talk || !vibe_recording_followup_present(window) ||
        now_ms > window->deadline_ms) {
        vibe_recording_followup_clear(window);
        return false;
    }
    return true;
}
