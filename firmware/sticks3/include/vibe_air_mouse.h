#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VIBE_AIR_MOUSE_CALIBRATION_SAMPLES 500
#define VIBE_AIR_MOUSE_DEFAULT_WHEEL_DEGREES_PER_TICK 8.0f

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
    uint8_t wheel_axis;
    int8_t wheel_sign;
    float pointer_deadzone_dps;
    float wheel_deadzone_dps;
    float wheel_degrees_per_tick;
} vibe_air_mouse_config_t;

typedef struct {
    float gyro_bias[3];
} vibe_air_mouse_calibration_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    int8_t wheel;
} vibe_air_mouse_output_t;

typedef struct {
    vibe_air_mouse_config_t config;
    float gyro_bias_sum[3];
    float gyro_bias[3];
    float previous_accel_g[3];
    float previous_gyro_dps[3];
    float filtered_horizontal_dps;
    float filtered_vertical_dps;
    float horizontal_remainder;
    float vertical_remainder;
    float wheel_degrees;
    uint16_t calibration_samples;
    bool has_previous_sample;
    bool calibrated;
} vibe_air_mouse_t;

extern const vibe_air_mouse_config_t
    VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG;

void vibe_air_mouse_init(vibe_air_mouse_t *mouse,
                         const vibe_air_mouse_config_t *config);
bool vibe_air_mouse_set_config(vibe_air_mouse_t *mouse,
                               const vibe_air_mouse_config_t *config);
void vibe_air_mouse_reset_motion(vibe_air_mouse_t *mouse);
void vibe_air_mouse_start_calibration(vibe_air_mouse_t *mouse);
bool vibe_air_mouse_set_calibration(
    vibe_air_mouse_t *mouse,
    const vibe_air_mouse_calibration_t *calibration);
bool vibe_air_mouse_get_calibration(
    const vibe_air_mouse_t *mouse,
    vibe_air_mouse_calibration_t *calibration);
bool vibe_air_mouse_update(vibe_air_mouse_t *mouse,
                           const vibe_air_mouse_sample_t *sample,
                           float delta_seconds,
                           vibe_air_mouse_output_t *output);
bool vibe_air_mouse_calibrated(const vibe_air_mouse_t *mouse);
uint16_t vibe_air_mouse_calibration_progress(const vibe_air_mouse_t *mouse);
