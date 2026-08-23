#include "vibe_capture_profile.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

static uint8_t register_value(const vibe_capture_profile_t *profile, uint8_t reg)
{
    for (size_t i = 0; i < profile->es8311_register_count; ++i) {
        if (profile->es8311_registers[i].reg == reg) {
            return profile->es8311_registers[i].value;
        }
    }
    assert(!"missing register");
    return 0;
}

static void test_profiles(void)
{
    const vibe_capture_profile_t *s3 =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKS3);
    const vibe_capture_profile_t *plus =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKC_PLUS);
    const vibe_capture_profile_t *plus_se =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKC_PLUS_SE);
    const vibe_capture_profile_t *cardputer =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_CARDPUTER_ADV);

    assert(s3 && plus && plus_se && cardputer);
    assert(s3->transport == VIBE_CAPTURE_TRANSPORT_ES8311_I2S);
    assert(s3->mclk_pin == 18 && s3->bclk_pin == 17 && s3->lrck_pin == 15);
    assert(s3->data_out_pin == 14 && s3->data_in_pin == 16);
    assert(s3->output_sample_rate == 16000);
    assert(s3->oversampling == 2 && s3->magnification == 16);
    assert(s3->noise_filter_level == 0);
    assert(vibe_capture_input_sample_rate(s3) == 32000);
    assert(register_value(s3, 0x17) == 0xFF);

    assert(plus->transport == VIBE_CAPTURE_TRANSPORT_PDM);
    assert(plus->pdm_clk_pin == 0 && plus->pdm_data_pin == 34);
    assert(plus->microphone_rail_mv == 2800);
    assert(plus->oversampling == 2 && plus->magnification == 16);
    assert(plus->noise_filter_level == 0);
    assert(vibe_capture_input_sample_rate(plus) == 32000);

    assert(plus_se->transport == VIBE_CAPTURE_TRANSPORT_PDM);
    assert(plus_se->pdm_clk_pin == plus->pdm_clk_pin);
    assert(plus_se->pdm_data_pin == plus->pdm_data_pin);
    assert(plus_se->microphone_rail_mv == plus->microphone_rail_mv);
    assert(plus_se->oversampling == plus->oversampling);
    assert(plus_se->magnification == plus->magnification);
    assert(plus_se->noise_filter_level == plus->noise_filter_level);

    assert(cardputer->transport == VIBE_CAPTURE_TRANSPORT_ES8311_I2S);
    assert(cardputer->mclk_pin == -1 && cardputer->bclk_pin == 41);
    assert(cardputer->lrck_pin == 43 && cardputer->data_out_pin == 42);
    assert(cardputer->data_in_pin == 46);
    assert(cardputer->oversampling == 1 && cardputer->magnification == 1);
    assert(cardputer->noise_filter_level == 0);
    assert(cardputer->input_gain_db == 36);
    assert(!cardputer->input_only_right && !cardputer->process_samples);
    assert(vibe_capture_input_sample_rate(cardputer) == 16000);
    assert(register_value(cardputer, 0x17) == 0xBF);
}

static void test_processing(void)
{
    const vibe_capture_profile_t *s3 =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_STICKS3);
    const vibe_capture_profile_t *cardputer =
        vibe_capture_profile_for(VIBE_CAPTURE_BOARD_CARDPUTER_ADV);
    vibe_capture_processor_t processor = {0};
    int16_t output[4] = {0};

    const int16_t silence[] = {0, 0, 0, 0};
    assert(vibe_capture_process_pcm16_mono(&processor, s3, silence, 4,
                                            output, 4) == 2);
    assert(output[0] == 0 && output[1] == 0);

    vibe_capture_processor_reset(&processor);
    const int16_t signal[] = {1024, 1024, 1024, 1024};
    assert(vibe_capture_process_pcm16_mono(&processor, s3, signal, 4,
                                            output, 4) == 2);
    const int16_t s3_level = output[1];
    assert(s3_level > 0);

    assert(!cardputer->process_samples);

    vibe_capture_processor_reset(&processor);
    const int16_t partial[] = {100, 100, 100};
    assert(vibe_capture_process_pcm16_mono(&processor, s3, partial, 3,
                                            output, 4) == 1);
}

int main(void)
{
    test_profiles();
    test_processing();
    puts("vibe capture profile tests passed");
    return 0;
}
