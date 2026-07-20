#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*vibe_recording_upload_post_fn)(const uint8_t *audio,
                                                  size_t audio_len,
                                                  void *context);

typedef struct {
    size_t buffer_bytes;
    size_t batch_chunks;
    uint32_t read_timeout_ms;
    uint32_t task_stack_bytes;
    unsigned int task_priority;
    int task_core;
    vibe_recording_upload_post_fn post_chunk;
    void *context;
} vibe_recording_upload_config_t;

bool vibe_recording_upload_start(const vibe_recording_upload_config_t *config,
                                 int start_rssi,
                                 int unknown_rssi);
void vibe_recording_upload_wait(void);
bool vibe_recording_upload_active(void);
bool vibe_recording_upload_failed(void);
void vibe_recording_upload_log_diagnostics(const char *board_name, int stop_rssi);
