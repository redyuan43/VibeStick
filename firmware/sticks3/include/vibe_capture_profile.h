#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIBE_CAPTURE_BOARD_STICKS3,
    VIBE_CAPTURE_BOARD_STICKC_PLUS,
    VIBE_CAPTURE_BOARD_STICKC_PLUS_SE,
    VIBE_CAPTURE_BOARD_CARDPUTER_ADV,
} vibe_capture_board_t;

typedef enum {
    VIBE_CAPTURE_TRANSPORT_ES8311_I2S,
    VIBE_CAPTURE_TRANSPORT_PDM,
} vibe_capture_transport_t;

typedef struct {
    uint8_t reg;
    uint8_t value;
} vibe_capture_es8311_register_t;

typedef struct {
    vibe_capture_board_t board;
    const char *board_name;
    vibe_capture_transport_t transport;
    uint32_t output_sample_rate;
    uint8_t oversampling;
    uint8_t magnification;
    uint8_t noise_filter_level;
    uint8_t input_gain_db;
    bool input_only_right;
    bool process_samples;
    int mclk_pin;
    int bclk_pin;
    int lrck_pin;
    int data_out_pin;
    int data_in_pin;
    int pdm_clk_pin;
    int pdm_data_pin;
    uint16_t microphone_rail_mv;
    const vibe_capture_es8311_register_t *es8311_registers;
    size_t es8311_register_count;
} vibe_capture_profile_t;

typedef struct {
    int32_t dc_offset;
    int32_t filtered_sample;
} vibe_capture_processor_t;

const vibe_capture_profile_t *vibe_capture_profile_for(vibe_capture_board_t board);
const vibe_capture_profile_t *vibe_capture_profile_current(void);
uint32_t vibe_capture_input_sample_rate(const vibe_capture_profile_t *profile);
void vibe_capture_processor_reset(vibe_capture_processor_t *processor);
size_t vibe_capture_process_pcm16_mono(vibe_capture_processor_t *processor,
                                       const vibe_capture_profile_t *profile,
                                       const int16_t *input, size_t input_samples,
                                       int16_t *output, size_t output_capacity);
