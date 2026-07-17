#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define VIBE_STICK_AUDIO_SAMPLE_RATE 16000
#define VIBE_STICK_AUDIO_CHANNELS 1
#define VIBE_STICK_AUDIO_BITS_PER_SAMPLE 16

typedef enum {
    VIBE_STICK_SOUND_DONE,
    VIBE_STICK_SOUND_ERROR,
    VIBE_STICK_SOUND_APPROVAL,
    VIBE_STICK_SOUND_RECORDING_START,
    VIBE_STICK_SOUND_RECORDING_STOP,
    VIBE_STICK_SOUND_FOLLOWUP_ENTER,
    VIBE_STICK_SOUND_FOLLOWUP_ESCAPE,
} agent_sound_t;

typedef struct {
    size_t chunks_read;
    size_t chunks_queued;
    size_t chunks_dropped;
    size_t bytes_read;
    size_t bytes_queued;
    size_t bytes_dropped;
} vibe_audio_stats_t;

esp_err_t vibe_audio_init(void);
esp_err_t vibe_audio_prepare_deep_sleep(void);
esp_err_t vibe_audio_start(void);
esp_err_t vibe_audio_stop(void);
esp_err_t vibe_audio_play_sound(agent_sound_t sound);
esp_err_t vibe_audio_play_pcm16_mono(const uint8_t *pcm, size_t len);
esp_err_t vibe_audio_read(uint8_t *buffer, size_t capacity, size_t *len, uint32_t timeout_ms);
esp_err_t vibe_audio_read_batch(uint8_t *buffer, size_t capacity, size_t *len,
                                size_t max_chunks, uint32_t timeout_ms);
size_t vibe_audio_pending_chunks(void);
void vibe_audio_stats(vibe_audio_stats_t *stats);
bool vibe_audio_is_recording(void);
const uint8_t *vibe_audio_data(size_t *len);
void vibe_audio_clear(void);
