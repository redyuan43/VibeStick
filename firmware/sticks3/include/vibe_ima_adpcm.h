#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_IMA_ADPCM_BLOCK_HEADER_BYTES 4

typedef struct {
    int16_t predictor;
    uint8_t step_index;
    bool initialized;
} vibe_ima_adpcm_state_t;

void vibe_ima_adpcm_reset(vibe_ima_adpcm_state_t *state);
size_t vibe_ima_adpcm_encoded_size(size_t sample_count);
bool vibe_ima_adpcm_encode_block(vibe_ima_adpcm_state_t *state,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint8_t *output,
                                 size_t output_capacity,
                                 size_t *output_len);
