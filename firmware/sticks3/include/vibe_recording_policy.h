#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_RECORDING_POLICY_SESSION_ID_LEN 40

typedef struct {
    char session_id[VIBE_RECORDING_POLICY_SESSION_ID_LEN];
    int64_t deadline_ms;
} vibe_recording_followup_window_t;

typedef struct {
    size_t upload_posts;
    size_t uploaded_bytes;
    size_t upload_failures;
    size_t read_failures;
    size_t read_timeouts;
    size_t max_pending_chunks;
    int64_t post_duration_total_ms;
    int64_t post_duration_min_ms;
    int64_t post_duration_max_ms;
    int start_rssi;
    int stop_rssi;
} vibe_recording_upload_stats_t;

void vibe_recording_followup_clear(vibe_recording_followup_window_t *window);
bool vibe_recording_followup_arm(vibe_recording_followup_window_t *window,
                                 const char *session_id,
                                 int64_t now_ms,
                                 int64_t grace_ms);
bool vibe_recording_followup_present(const vibe_recording_followup_window_t *window);
bool vibe_recording_followup_consume(vibe_recording_followup_window_t *window,
                                     bool push_to_talk,
                                     int64_t now_ms);

void vibe_recording_upload_stats_reset(vibe_recording_upload_stats_t *stats,
                                       int start_rssi,
                                       int unknown_rssi);
void vibe_recording_upload_stats_note_pending(vibe_recording_upload_stats_t *stats,
                                              size_t pending_chunks);
void vibe_recording_upload_stats_note_read(vibe_recording_upload_stats_t *stats,
                                           bool timed_out);
void vibe_recording_upload_stats_note_post(vibe_recording_upload_stats_t *stats,
                                           int64_t duration_ms,
                                           size_t uploaded_bytes,
                                           bool succeeded);
int64_t vibe_recording_upload_stats_average_post_ms(
    const vibe_recording_upload_stats_t *stats);
