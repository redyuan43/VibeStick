#include "vibe_audio_pdm_filter.h"

#include <limits.h>

#define DC_TRACKING_DIVISOR 32

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

void vibe_audio_pdm_filter_reset(vibe_audio_pdm_filter_t *filter)
{
    if (filter == NULL) {
        return;
    }
    filter->dc_estimate_q8 = 0;
    filter->previous_output = 0;
}

size_t vibe_audio_pdm_filter_process(vibe_audio_pdm_filter_t *filter,
                                     const int16_t *input,
                                     size_t input_samples,
                                     int16_t *output,
                                     size_t output_capacity,
                                     uint8_t oversampling,
                                     uint8_t noise_filter_level)
{
    if (filter == NULL || input == NULL || output == NULL || oversampling == 0) {
        return 0;
    }

    size_t output_samples = 0;
    size_t input_index = 0;
    while (input_index + oversampling <= input_samples &&
           output_samples < output_capacity) {
        int32_t sum = 0;
        for (uint8_t index = 0; index < oversampling; ++index) {
            sum += input[input_index++];
        }

        int32_t sample = sum / oversampling;
        int32_t sample_q8 = sample * 256;
        filter->dc_estimate_q8 +=
            (sample_q8 - filter->dc_estimate_q8) / DC_TRACKING_DIVISOR;
        sample -= filter->dc_estimate_q8 / 256;

        if (noise_filter_level != 0) {
            sample = (sample * (256 - noise_filter_level) +
                      filter->previous_output * noise_filter_level) /
                     256;
        }

        sample = clamp_i16(sample);
        filter->previous_output = sample;
        output[output_samples++] = (int16_t)sample;
    }
    return output_samples;
}
