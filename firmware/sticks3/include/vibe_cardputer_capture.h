#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define VIBE_CARDPUTER_CAPTURE_SAMPLE_RATE 16000
#define VIBE_CARDPUTER_CAPTURE_FRAME_BYTES 1920
#define VIBE_CARDPUTER_CAPTURE_FRAME_SAMPLES \
    (VIBE_CARDPUTER_CAPTURE_FRAME_BYTES / sizeof(int16_t))
#define VIBE_CARDPUTER_CAPTURE_ENCODED_FRAME_BYTES 484
#define VIBE_CARDPUTER_CAPTURE_AUDIO_ENCODING "ima-adpcm-v1"
#define VIBE_CARDPUTER_CAPTURE_CONTENT_TYPE \
    "application/vnd.vibestick.ima-adpcm"

typedef struct {
    size_t chunks_read;
    size_t chunks_queued;
    size_t chunks_dropped;
    size_t bytes_read;
    size_t bytes_dropped;
} vibe_cardputer_capture_stats_t;

esp_err_t vibe_cardputer_capture_init(void);
esp_err_t vibe_cardputer_capture_start(void);
esp_err_t vibe_cardputer_capture_stop(void);
bool vibe_cardputer_capture_is_recording(void);
esp_err_t vibe_cardputer_capture_read_batch(uint8_t *buffer, size_t capacity,
                                            size_t *len, size_t max_chunks,
                                            uint32_t timeout_ms);
size_t vibe_cardputer_capture_pending_chunks(void);
void vibe_cardputer_capture_stats(vibe_cardputer_capture_stats_t *stats);
void vibe_cardputer_capture_clear(void);
