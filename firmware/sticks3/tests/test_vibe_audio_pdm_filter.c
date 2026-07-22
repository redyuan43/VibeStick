#include <assert.h>
#include <stdint.h>

#include "vibe_audio_pdm_filter.h"

static void test_invalid_arguments(void)
{
    vibe_audio_pdm_filter_t filter = {0};
    int16_t input[] = {1, 2};
    int16_t output[1];

    assert(vibe_audio_pdm_filter_process(NULL, input, 2, output, 1, 2, 0) == 0);
    assert(vibe_audio_pdm_filter_process(&filter, NULL, 2, output, 1, 2, 0) == 0);
    assert(vibe_audio_pdm_filter_process(&filter, input, 2, NULL, 1, 2, 0) == 0);
    assert(vibe_audio_pdm_filter_process(&filter, input, 2, output, 1, 0, 0) == 0);
}

static void test_oversampling_rejects_opposite_noise(void)
{
    vibe_audio_pdm_filter_t filter = {0};
    int16_t input[] = {1200, -1200, 800, -800, 400, -400};
    int16_t output[3] = {1, 1, 1};

    size_t count = vibe_audio_pdm_filter_process(
        &filter, input, 6, output, 3, 2, 0);
    assert(count == 3);
    assert(output[0] == 0);
    assert(output[1] == 0);
    assert(output[2] == 0);
}

static void test_dc_offset_converges(void)
{
    vibe_audio_pdm_filter_t filter = {0};
    int16_t input[512];
    int16_t output[256];
    for (size_t index = 0; index < 512; ++index) {
        input[index] = 1000;
    }

    size_t count = vibe_audio_pdm_filter_process(
        &filter, input, 512, output, 256, 2, 0);
    assert(count == 256);
    assert(output[0] == 969);
    assert(output[255] >= 0);
    assert(output[255] < 10);
}

static void test_noise_filter_smooths_step(void)
{
    vibe_audio_pdm_filter_t unfiltered = {0};
    vibe_audio_pdm_filter_t filtered = {0};
    int16_t input[] = {1000, 1000, -1000, -1000};
    int16_t raw_output[2];
    int16_t smooth_output[2];

    assert(vibe_audio_pdm_filter_process(
               &unfiltered, input, 4, raw_output, 2, 2, 0) == 2);
    assert(vibe_audio_pdm_filter_process(
               &filtered, input, 4, smooth_output, 2, 2, 32) == 2);
    assert(smooth_output[0] < raw_output[0]);
    assert(smooth_output[1] > raw_output[1]);
}

static void test_output_capacity_is_respected(void)
{
    vibe_audio_pdm_filter_t filter = {0};
    int16_t input[] = {10, 10, 20, 20, 30, 30};
    int16_t output[2];

    assert(vibe_audio_pdm_filter_process(
               &filter, input, 6, output, 2, 2, 0) == 2);
}

int main(void)
{
    test_invalid_arguments();
    test_oversampling_rejects_opposite_noise();
    test_dc_offset_converges();
    test_noise_filter_smooths_step();
    test_output_capacity_is_respected();
    return 0;
}
