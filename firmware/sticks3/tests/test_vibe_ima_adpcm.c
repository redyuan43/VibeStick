#include "vibe_ima_adpcm.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
static const int8_t s_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int clamp_int(int value, int minimum, int maximum)
{
    return value < minimum ? minimum :
           value > maximum ? maximum : value;
}

static void decode(const uint8_t *encoded, size_t sample_count,
                   int16_t *samples)
{
    int predictor = (int16_t)((uint16_t)encoded[0] |
                              ((uint16_t)encoded[1] << 8));
    int step_index = encoded[2];
    samples[0] = (int16_t)predictor;
    for (size_t index = 1; index < sample_count; ++index) {
        const uint8_t packed =
            encoded[VIBE_IMA_ADPCM_BLOCK_HEADER_BYTES + (index - 1) / 2];
        const uint8_t code =
            (index & 1) ? packed & 0x0f : packed >> 4;
        const int step = s_step_table[step_index];
        int delta = step >> 3;
        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;
        predictor += (code & 8) ? -delta : delta;
        predictor = clamp_int(predictor, INT16_MIN, INT16_MAX);
        step_index = clamp_int(
            step_index + s_index_table[code & 7], 0, 88);
        samples[index] = (int16_t)predictor;
    }
}

int main(void)
{
    enum { SAMPLE_COUNT = 960, ENCODED_BYTES = 484 };
    int16_t input[SAMPLE_COUNT] = {0};
    int16_t output[SAMPLE_COUNT] = {0};
    uint8_t encoded[ENCODED_BYTES] = {0};
    size_t encoded_len = 0;
    vibe_ima_adpcm_state_t state = {0};

    assert(vibe_ima_adpcm_encoded_size(SAMPLE_COUNT) == ENCODED_BYTES);
    vibe_ima_adpcm_reset(&state);
    assert(vibe_ima_adpcm_encode_block(
        &state, input, SAMPLE_COUNT, encoded, sizeof(encoded), &encoded_len));
    assert(encoded_len == ENCODED_BYTES);
    decode(encoded, SAMPLE_COUNT, output);
    assert(memcmp(input, output, sizeof(input)) == 0);

    for (size_t index = 0; index < SAMPLE_COUNT; ++index) {
        input[index] = (int16_t)((int)(index % 160) * 200 - 16000);
    }
    vibe_ima_adpcm_reset(&state);
    assert(vibe_ima_adpcm_encode_block(
        &state, input, SAMPLE_COUNT, encoded, sizeof(encoded), &encoded_len));
    decode(encoded, SAMPLE_COUNT, output);
    long absolute_error = 0;
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) {
        absolute_error += labs((long)input[index] - output[index]);
    }
    assert(absolute_error / SAMPLE_COUNT < 900);
    return 0;
}
