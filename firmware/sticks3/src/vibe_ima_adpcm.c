#include "vibe_ima_adpcm.h"

#include <limits.h>

static const int s_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,
    17,    19,    21,    23,    25,    28,    31,    34,    37,
    41,    45,    50,    55,    60,    66,    73,    80,    88,
    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,
    1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,
    3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t s_index_table[8] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint8_t estimate_initial_index(const int16_t *samples,
                                      size_t sample_count)
{
    if (sample_count < 2) {
        return 0;
    }
    size_t differences = sample_count - 1;
    if (differences > 32) {
        differences = 32;
    }
    uint32_t total = 0;
    for (size_t index = 0; index < differences; ++index) {
        int difference = (int)samples[index + 1] - samples[index];
        total += (uint32_t)(difference < 0 ? -difference : difference);
    }
    const uint32_t average = total / differences;
    for (uint8_t index = 0; index < 88; ++index) {
        if ((uint32_t)s_step_table[index] >= average) {
            return index;
        }
    }
    return 88;
}

static uint8_t encode_sample(vibe_ima_adpcm_state_t *state, int16_t sample)
{
    int predictor = state->predictor;
    int difference = (int)sample - predictor;
    uint8_t code = 0;
    if (difference < 0) {
        code = 8;
        difference = -difference;
    }

    const int step = s_step_table[state->step_index];
    int delta = step >> 3;
    if (difference >= step) {
        code |= 4;
        difference -= step;
        delta += step;
    }
    if (difference >= (step >> 1)) {
        code |= 2;
        difference -= step >> 1;
        delta += step >> 1;
    }
    if (difference >= (step >> 2)) {
        code |= 1;
        delta += step >> 2;
    }

    predictor += (code & 8) ? -delta : delta;
    predictor = clamp_int(predictor, INT16_MIN, INT16_MAX);
    int step_index = state->step_index + s_index_table[code & 7];
    state->predictor = (int16_t)predictor;
    state->step_index = (uint8_t)clamp_int(step_index, 0, 88);
    return code;
}

void vibe_ima_adpcm_reset(vibe_ima_adpcm_state_t *state)
{
    if (!state) {
        return;
    }
    state->predictor = 0;
    state->step_index = 0;
    state->initialized = false;
}

size_t vibe_ima_adpcm_encoded_size(size_t sample_count)
{
    if (sample_count == 0) {
        return 0;
    }
    return VIBE_IMA_ADPCM_BLOCK_HEADER_BYTES + sample_count / 2;
}

bool vibe_ima_adpcm_encode_block(vibe_ima_adpcm_state_t *state,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint8_t *output,
                                 size_t output_capacity,
                                 size_t *output_len)
{
    if (!state || !samples || !output || !output_len || sample_count == 0) {
        return false;
    }
    const size_t encoded_size = vibe_ima_adpcm_encoded_size(sample_count);
    if (output_capacity < encoded_size) {
        return false;
    }

    state->predictor = samples[0];
    if (!state->initialized) {
        state->step_index = estimate_initial_index(samples, sample_count);
        state->initialized = true;
    }
    output[0] = (uint8_t)(state->predictor & 0xff);
    output[1] = (uint8_t)(((uint16_t)state->predictor >> 8) & 0xff);
    output[2] = state->step_index;
    output[3] = 0;

    size_t output_index = VIBE_IMA_ADPCM_BLOCK_HEADER_BYTES;
    uint8_t packed = 0;
    bool low_nibble = true;
    for (size_t sample_index = 1; sample_index < sample_count;
         ++sample_index) {
        const uint8_t code = encode_sample(state, samples[sample_index]);
        if (low_nibble) {
            packed = code;
            low_nibble = false;
        } else {
            output[output_index++] = packed | (uint8_t)(code << 4);
            low_nibble = true;
        }
    }
    if (!low_nibble) {
        output[output_index++] = packed;
    }
    *output_len = output_index;
    return output_index == encoded_size;
}
