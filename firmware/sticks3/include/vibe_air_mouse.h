#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
} vibe_air_mouse_sample_t;

typedef struct {
    uint8_t horizontal_axis;
    int8_t horizontal_sign;
    float horizontal_gain;
    uint8_t vertical_axis;
    int8_t vertical_sign;
    float vertical_gain;
} vibe_air_mouse_config_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    int8_t wheel;
    bool motion_started;
} vibe_air_mouse_output_t;

typedef struct {
    vibe_air_mouse_config_t config;
    float accel_baseline_sum[3];
    float accel_baseline[3];
    float gyro_bias_sum[3];
    float gyro_bias[3];
    float previous_calibration_accel_g[3];
    float previous_calibration_gyro_dps[3];
    float filtered_horizontal_dps;
    float filtered_vertical_dps;
    float horizontal_remainder;
    float vertical_remainder;
    float filtered_pitch_degrees;
    float tilt_candidate_seconds;
    float tilt_neutral_seconds;
    float tilt_repeat_seconds;
    float motion_candidate_seconds;
    float motion_quiet_seconds;
    uint16_t calibration_samples;
    int8_t tilt_candidate_direction;
    int8_t tilt_active_direction;
    bool has_previous_calibration_sample;
    bool tilt_armed;
    bool motion_sound_armed;
    bool calibrated;
} vibe_air_mouse_t;

void vibe_air_mouse_init(vibe_air_mouse_t *mouse,
                         const vibe_air_mouse_config_t *config);
void vibe_air_mouse_reset_motion(vibe_air_mouse_t *mouse);
bool vibe_air_mouse_update(vibe_air_mouse_t *mouse,
                           const vibe_air_mouse_sample_t *sample,
                           float delta_seconds,
                           vibe_air_mouse_output_t *output);
bool vibe_air_mouse_calibrated(const vibe_air_mouse_t *mouse);
uint16_t vibe_air_mouse_calibration_progress(const vibe_air_mouse_t *mouse);
