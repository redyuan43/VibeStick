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

void vibe_recording_upload_stats_reset(vibe_recording_upload_stats_t *stats,
                                       int start_rssi,
                                       int unknown_rssi)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->post_duration_min_ms = -1;
    stats->start_rssi = start_rssi;
    stats->stop_rssi = unknown_rssi;
}

void vibe_recording_upload_stats_note_pending(vibe_recording_upload_stats_t *stats,
                                              size_t pending_chunks)
{
    if (stats && pending_chunks > stats->max_pending_chunks) {
        stats->max_pending_chunks = pending_chunks;
    }
}

void vibe_recording_upload_stats_note_read(vibe_recording_upload_stats_t *stats,
                                           bool timed_out)
{
    if (!stats) {
        return;
    }
    if (timed_out) {
        stats->read_timeouts++;
    } else {
        stats->read_failures++;
    }
}

void vibe_recording_upload_stats_note_post(vibe_recording_upload_stats_t *stats,
                                           int64_t duration_ms,
                                           size_t uploaded_bytes,
                                           bool succeeded)
{
    if (!stats) {
        return;
    }
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    if (stats->post_duration_min_ms < 0 ||
        duration_ms < stats->post_duration_min_ms) {
        stats->post_duration_min_ms = duration_ms;
    }
    if (duration_ms > stats->post_duration_max_ms) {
        stats->post_duration_max_ms = duration_ms;
    }
    stats->post_duration_total_ms += duration_ms;
    if (succeeded) {
        stats->upload_posts++;
        stats->uploaded_bytes += uploaded_bytes;
    } else {
        stats->upload_failures++;
    }
}

int64_t vibe_recording_upload_stats_average_post_ms(
    const vibe_recording_upload_stats_t *stats)
{
    if (!stats || stats->upload_posts == 0) {
        return 0;
    }
    return stats->post_duration_total_ms / (int64_t)stats->upload_posts;
}
