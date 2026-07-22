#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t dc_estimate_q8;
    int32_t previous_output;
} vibe_audio_pdm_filter_t;

void vibe_audio_pdm_filter_reset(vibe_audio_pdm_filter_t *filter);

size_t vibe_audio_pdm_filter_process(vibe_audio_pdm_filter_t *filter,
                                     const int16_t *input,
                                     size_t input_samples,
                                     int16_t *output,
                                     size_t output_capacity,
                                     uint8_t oversampling,
                                     uint8_t noise_filter_level);
