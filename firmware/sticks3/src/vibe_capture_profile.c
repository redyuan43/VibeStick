#include "vibe_capture_profile.h"

#include <limits.h>

static const vibe_capture_es8311_register_t s_sticks3_es8311_registers[] = {
    {0x00, 0x80},
    {0x01, 0xBA},
    {0x02, 0x18},
    {0x0D, 0x01},
    {0x0E, 0x02},
    {0x14, 0x10},
    {0x17, 0xFF},
    {0x1C, 0x6A},
};

static const vibe_capture_es8311_register_t s_cardputer_es8311_registers[] = {
    {0x00, 0x80},
    {0x01, 0xBA},
    {0x02, 0x18},
    {0x0D, 0x01},
    {0x0E, 0x02},
    {0x14, 0x10},
    {0x17, 0xBF},
    {0x1C, 0x6A},
};

static const vibe_capture_profile_t s_profiles[] = {
    {
        .board = VIBE_CAPTURE_BOARD_STICKS3,
        .board_name = "sticks3",
        .transport = VIBE_CAPTURE_TRANSPORT_ES8311_I2S,
        .output_sample_rate = 16000,
        .oversampling = 2,
        .magnification = 16,
        .noise_filter_level = 0,
        .input_gain_db = 0,
        .input_only_right = true,
        .process_samples = true,
        .mclk_pin = 18,
        .bclk_pin = 17,
        .lrck_pin = 15,
        .data_out_pin = 14,
        .data_in_pin = 16,
        .pdm_clk_pin = -1,
        .pdm_data_pin = -1,
        .microphone_rail_mv = 0,
        .es8311_registers = s_sticks3_es8311_registers,
        .es8311_register_count = sizeof(s_sticks3_es8311_registers) /
                                  sizeof(s_sticks3_es8311_registers[0]),
    },
    {
        .board = VIBE_CAPTURE_BOARD_STICKC_PLUS,
        .board_name = "stickc_plus",
        .transport = VIBE_CAPTURE_TRANSPORT_PDM,
        .output_sample_rate = 16000,
        .oversampling = 2,
        .magnification = 16,
        .noise_filter_level = 0,
        .input_gain_db = 0,
        .input_only_right = true,
        .process_samples = true,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .lrck_pin = -1,
        .data_out_pin = -1,
        .data_in_pin = -1,
        .pdm_clk_pin = 0,
        .pdm_data_pin = 34,
        .microphone_rail_mv = 2800,
        .es8311_registers = NULL,
        .es8311_register_count = 0,
    },
    {
        .board = VIBE_CAPTURE_BOARD_STICKC_PLUS_SE,
        .board_name = "stickc_plus_se",
        .transport = VIBE_CAPTURE_TRANSPORT_PDM,
        .output_sample_rate = 16000,
        .oversampling = 2,
        .magnification = 16,
        .noise_filter_level = 0,
        .input_gain_db = 0,
        .input_only_right = true,
        .process_samples = true,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .lrck_pin = -1,
        .data_out_pin = -1,
        .data_in_pin = -1,
        .pdm_clk_pin = 0,
        .pdm_data_pin = 34,
        .microphone_rail_mv = 2800,
        .es8311_registers = NULL,
        .es8311_register_count = 0,
    },
    {
        .board = VIBE_CAPTURE_BOARD_CARDPUTER_ADV,
        .board_name = "cardputer_adv",
        .transport = VIBE_CAPTURE_TRANSPORT_ES8311_I2S,
        .output_sample_rate = 16000,
        /*
         * Cardputer's 32 kHz software capture experiment regressed interactive
         * responsiveness in device testing. Keep its proven direct 16 kHz path
         * until it can be reintroduced without competing with the keyboard/UI.
         */
        .oversampling = 1,
        .magnification = 1,
        .noise_filter_level = 0,
        .input_gain_db = 36,
        .input_only_right = false,
        .process_samples = false,
        .mclk_pin = -1,
        .bclk_pin = 41,
        .lrck_pin = 43,
        .data_out_pin = 42,
        .data_in_pin = 46,
        .pdm_clk_pin = -1,
        .pdm_data_pin = -1,
        .microphone_rail_mv = 0,
        .es8311_registers = s_cardputer_es8311_registers,
        .es8311_register_count = sizeof(s_cardputer_es8311_registers) /
                                  sizeof(s_cardputer_es8311_registers[0]),
    },
};

const vibe_capture_profile_t *vibe_capture_profile_for(vibe_capture_board_t board)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); ++i) {
        if (s_profiles[i].board == board) {
            return &s_profiles[i];
        }
    }
    return NULL;
}

const vibe_capture_profile_t *vibe_capture_profile_current(void)
{
#if defined(VIBE_BOARD_STICKC_PLUS)
    return vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKC_PLUS);
#elif defined(VIBE_BOARD_STICKC_PLUS_SE)
    return vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKC_PLUS_SE);
#elif defined(VIBE_BOARD_CARDPUTER_ADV)
    return vibe_capture_profile_for(VIBE_CAPTURE_BOARD_CARDPUTER_ADV);
#else
    return vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKS3);
#endif
}

uint32_t vibe_capture_input_sample_rate(const vibe_capture_profile_t *profile)
{
    if (!profile || profile->oversampling == 0) {
        return 0;
    }
    return profile->output_sample_rate * profile->oversampling;
}

void vibe_capture_processor_reset(vibe_capture_processor_t *processor)
{
    if (!processor) {
        return;
    }
    processor->dc_offset = 0;
    processor->filtered_sample = 0;
}

static int32_t rounded_average(const int16_t *input, uint8_t count)
{
    int32_t sum = 0;
    for (uint8_t i = 0; i < count; ++i) {
        sum += input[i];
    }
    if (sum >= 0) {
        return (sum + count / 2) / count;
    }
    return (sum - count / 2) / count;
}

static int16_t saturate_pcm16(int32_t sample)
{
    if (sample > INT16_MAX - 16) {
        return INT16_MAX - 16;
    }
    if (sample < INT16_MIN + 16) {
        return INT16_MIN + 16;
    }
    return (int16_t)sample;
}

size_t vibe_capture_process_pcm16_mono(vibe_capture_processor_t *processor,
                                       const vibe_capture_profile_t *profile,
                                       const int16_t *input, size_t input_samples,
                                       int16_t *output, size_t output_capacity)
{
    if (!processor || !profile || !input || !output || profile->oversampling == 0) {
        return 0;
    }

    const uint8_t oversampling = profile->oversampling;
    const size_t available = input_samples / oversampling;
    const size_t samples = available < output_capacity ? available : output_capacity;
    const int32_t gain_divisor = (int32_t)oversampling * 2;

    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = rounded_average(input + i * oversampling, oversampling);

        // This is the mono equivalent of M5Unified's running DC offset cancellation.
        const int32_t tracked = sample << 4;
        processor->dc_offset -= (tracked + processor->dc_offset + 16) >> 5;
        sample += (processor->dc_offset + 8) >> 4;

        if (profile->noise_filter_level != 0) {
            const int32_t level = profile->noise_filter_level;
            sample = (sample * (256 - level) +
                      processor->filtered_sample * level + 128) >> 8;
            processor->filtered_sample = sample;
        }

        sample = (sample * profile->magnification) / gain_divisor;
        output[i] = saturate_pcm16(sample);
    }
    return samples;
}
