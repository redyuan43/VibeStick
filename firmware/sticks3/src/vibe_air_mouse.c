#include "vibe_air_mouse.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define AIR_MOUSE_STILL_ACCEL_TOLERANCE_G 0.15f
#define AIR_MOUSE_STILL_ACCEL_DELTA_G 0.05f
#define AIR_MOUSE_STILL_GYRO_DELTA_DPS 5.0f
#define AIR_MOUSE_MAX_GYRO_BIAS_DPS 120.0f
#define AIR_MOUSE_POINTER_DEADZONE_DPS 2.5f
#define AIR_MOUSE_WHEEL_DEADZONE_DPS 5.0f
#define AIR_MOUSE_FILTER_ALPHA 0.45f
#define AIR_MOUSE_LINEAR_PIXELS_PER_DEGREE 8.0f
#define AIR_MOUSE_ACCEL_PIXELS_PER_DEGREE_SQUARED 0.015f
#define AIR_MOUSE_WHEEL_DEGREES_PER_TICK 8.0f
#define AIR_MOUSE_DEFAULT_DELTA_SECONDS 0.02f
#define AIR_MOUSE_MAX_DELTA_SECONDS 0.05f

/*
 * M5Stack Cardputer-Adv official axes:
 * https://docs.m5stack.com/en/core/Cardputer-Adv
 * The factory demo integrates signed gyro.z for in-plane rotation:
 * https://github.com/m5stack/M5Cardputer-UserDemo/blob/CardputerADV/main/apps/app_imu/app_imu.cpp
 */
const vibe_air_mouse_config_t VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG = {
    .horizontal_axis = 2,
    .horizontal_sign = -1,
    .horizontal_gain = 1.8f,
    .vertical_axis = 0,
    .vertical_sign = 1,
    .vertical_gain = 1.0f,
    .wheel_axis = 1,
    .wheel_sign = -1,
    .pointer_deadzone_dps = AIR_MOUSE_POINTER_DEADZONE_DPS,
    .wheel_deadzone_dps = AIR_MOUSE_WHEEL_DEADZONE_DPS,
    .wheel_degrees_per_tick = AIR_MOUSE_WHEEL_DEGREES_PER_TICK,
};

static float vector_norm(const float vector[3])
{
    return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] +
                 vector[2] * vector[2]);
}

static bool valid_axis(uint8_t axis)
{
    return axis < 3;
}

static int8_t valid_sign(int8_t sign)
{
    return sign == -1 ? -1 : 1;
}

static bool valid_calibration(
    const vibe_air_mouse_calibration_t *calibration)
{
    if (!calibration) {
        return false;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!isfinite(calibration->gyro_bias[axis]) ||
            fabsf(calibration->gyro_bias[axis]) >
                AIR_MOUSE_MAX_GYRO_BIAS_DPS) {
            return false;
        }
    }
    return true;
}

static bool sample_is_stationary(const vibe_air_mouse_t *mouse,
                                 const vibe_air_mouse_sample_t *sample)
{
    if (fabsf(vector_norm(sample->accel_g) - 1.0f) >
            AIR_MOUSE_STILL_ACCEL_TOLERANCE_G ||
        vector_norm(sample->gyro_dps) > AIR_MOUSE_MAX_GYRO_BIAS_DPS) {
        return false;
    }
    if (!mouse->has_previous_sample) {
        return true;
    }

    float accel_delta[3];
    float gyro_delta[3];
    for (size_t axis = 0; axis < 3; ++axis) {
        accel_delta[axis] = sample->accel_g[axis] -
                            mouse->previous_accel_g[axis];
        gyro_delta[axis] = sample->gyro_dps[axis] -
                           mouse->previous_gyro_dps[axis];
    }
    return vector_norm(accel_delta) <= AIR_MOUSE_STILL_ACCEL_DELTA_G &&
           vector_norm(gyro_delta) <= AIR_MOUSE_STILL_GYRO_DELTA_DPS;
}

static void remember_sample(vibe_air_mouse_t *mouse,
                            const vibe_air_mouse_sample_t *sample)
{
    memcpy(mouse->previous_accel_g, sample->accel_g,
           sizeof(mouse->previous_accel_g));
    memcpy(mouse->previous_gyro_dps, sample->gyro_dps,
           sizeof(mouse->previous_gyro_dps));
    mouse->has_previous_sample = true;
}

static float apply_deadzone(float value, float deadzone)
{
    float magnitude = fabsf(value);
    if (magnitude <= deadzone) {
        return 0.0f;
    }
    return copysignf(magnitude - deadzone, value);
}

static float pointer_speed(float angular_rate_dps)
{
    return AIR_MOUSE_LINEAR_PIXELS_PER_DEGREE * angular_rate_dps +
           AIR_MOUSE_ACCEL_PIXELS_PER_DEGREE_SQUARED * angular_rate_dps *
               fabsf(angular_rate_dps);
}

static int8_t consume_pointer_axis(float delta, float *remainder)
{
    float accumulated = *remainder + delta;
    long whole = lroundf(accumulated);
    if (whole > INT8_MAX) {
        *remainder = 0.0f;
        return INT8_MAX;
    }
    if (whole < INT8_MIN) {
        *remainder = 0.0f;
        return INT8_MIN;
    }
    *remainder = accumulated - (float)whole;
    return (int8_t)whole;
}

static int8_t consume_wheel(vibe_air_mouse_t *mouse, float rate_dps,
                            float delta_seconds)
{
    mouse->wheel_degrees += rate_dps * delta_seconds;
    const float degrees_per_tick = mouse->config.wheel_degrees_per_tick;
    int8_t ticks = 0;
    while (mouse->wheel_degrees >= degrees_per_tick &&
           ticks < 4) {
        ++ticks;
        mouse->wheel_degrees -= degrees_per_tick;
    }
    while (mouse->wheel_degrees <= -degrees_per_tick &&
           ticks > -4) {
        --ticks;
        mouse->wheel_degrees += degrees_per_tick;
    }
    return ticks;
}

void vibe_air_mouse_init(vibe_air_mouse_t *mouse,
                         const vibe_air_mouse_config_t *config)
{
    if (!mouse || !config) {
        return;
    }
    memset(mouse, 0, sizeof(*mouse));
    (void)vibe_air_mouse_set_config(mouse, config);
}

bool vibe_air_mouse_set_config(vibe_air_mouse_t *mouse,
                               const vibe_air_mouse_config_t *config)
{
    if (!mouse || !config || !valid_axis(config->horizontal_axis) ||
        !valid_axis(config->vertical_axis) || !valid_axis(config->wheel_axis)) {
        return false;
    }
    mouse->config = *config;
    mouse->config.horizontal_sign = valid_sign(config->horizontal_sign);
    mouse->config.vertical_sign = valid_sign(config->vertical_sign);
    mouse->config.wheel_sign = valid_sign(config->wheel_sign);
    if (!(mouse->config.horizontal_gain > 0.0f) ||
        !isfinite(mouse->config.horizontal_gain)) {
        mouse->config.horizontal_gain = 1.0f;
    }
    if (!(mouse->config.vertical_gain > 0.0f) ||
        !isfinite(mouse->config.vertical_gain)) {
        mouse->config.vertical_gain = 1.0f;
    }
    if (!(mouse->config.pointer_deadzone_dps >= 1.0f) ||
        mouse->config.pointer_deadzone_dps > 6.0f ||
        !isfinite(mouse->config.pointer_deadzone_dps)) {
        mouse->config.pointer_deadzone_dps = AIR_MOUSE_POINTER_DEADZONE_DPS;
    }
    if (!(mouse->config.wheel_deadzone_dps >= 2.0f) ||
        mouse->config.wheel_deadzone_dps > 10.0f ||
        !isfinite(mouse->config.wheel_deadzone_dps)) {
        mouse->config.wheel_deadzone_dps = AIR_MOUSE_WHEEL_DEADZONE_DPS;
    }
    if (!(mouse->config.wheel_degrees_per_tick >= 4.0f) ||
        mouse->config.wheel_degrees_per_tick > 16.0f ||
        !isfinite(mouse->config.wheel_degrees_per_tick)) {
        mouse->config.wheel_degrees_per_tick = AIR_MOUSE_WHEEL_DEGREES_PER_TICK;
    }
    vibe_air_mouse_reset_motion(mouse);
    return true;
}

void vibe_air_mouse_reset_motion(vibe_air_mouse_t *mouse)
{
    if (!mouse) {
        return;
    }
    mouse->filtered_horizontal_dps = 0.0f;
    mouse->filtered_vertical_dps = 0.0f;
    mouse->horizontal_remainder = 0.0f;
    mouse->vertical_remainder = 0.0f;
    mouse->wheel_degrees = 0.0f;
}

void vibe_air_mouse_start_calibration(vibe_air_mouse_t *mouse)
{
    if (!mouse) {
        return;
    }
    memset(mouse->gyro_bias_sum, 0, sizeof(mouse->gyro_bias_sum));
    memset(mouse->gyro_bias, 0, sizeof(mouse->gyro_bias));
    memset(mouse->previous_accel_g, 0, sizeof(mouse->previous_accel_g));
    memset(mouse->previous_gyro_dps, 0, sizeof(mouse->previous_gyro_dps));
    mouse->calibration_samples = 0;
    mouse->has_previous_sample = false;
    mouse->calibrated = false;
    vibe_air_mouse_reset_motion(mouse);
}

bool vibe_air_mouse_set_calibration(
    vibe_air_mouse_t *mouse,
    const vibe_air_mouse_calibration_t *calibration)
{
    if (!mouse || !valid_calibration(calibration)) {
        return false;
    }
    memcpy(mouse->gyro_bias, calibration->gyro_bias,
           sizeof(mouse->gyro_bias));
    mouse->calibration_samples = VIBE_AIR_MOUSE_CALIBRATION_SAMPLES;
    mouse->has_previous_sample = false;
    mouse->calibrated = true;
    vibe_air_mouse_reset_motion(mouse);
    return true;
}

bool vibe_air_mouse_get_calibration(
    const vibe_air_mouse_t *mouse,
    vibe_air_mouse_calibration_t *calibration)
{
    if (!mouse || !mouse->calibrated || !calibration) {
        return false;
    }
    memcpy(calibration->gyro_bias, mouse->gyro_bias,
           sizeof(calibration->gyro_bias));
    return valid_calibration(calibration);
}

bool vibe_air_mouse_update(vibe_air_mouse_t *mouse,
                           const vibe_air_mouse_sample_t *sample,
                           float delta_seconds,
                           vibe_air_mouse_output_t *output)
{
    if (!mouse || !sample || !output ||
        !valid_axis(mouse->config.horizontal_axis) ||
        !valid_axis(mouse->config.vertical_axis) ||
        !valid_axis(mouse->config.wheel_axis)) {
        return false;
    }
    *output = (vibe_air_mouse_output_t){0};

    if (!mouse->calibrated) {
        if (!sample_is_stationary(mouse, sample)) {
            memset(mouse->gyro_bias_sum, 0, sizeof(mouse->gyro_bias_sum));
            mouse->calibration_samples = 0;
            mouse->has_previous_sample = false;
            return false;
        }
        remember_sample(mouse, sample);
        for (size_t axis = 0; axis < 3; ++axis) {
            mouse->gyro_bias_sum[axis] += sample->gyro_dps[axis];
        }
        ++mouse->calibration_samples;
        if (mouse->calibration_samples <
            VIBE_AIR_MOUSE_CALIBRATION_SAMPLES) {
            return false;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            mouse->gyro_bias[axis] =
                mouse->gyro_bias_sum[axis] /
                (float)mouse->calibration_samples;
        }
        mouse->calibrated = true;
        mouse->has_previous_sample = false;
        vibe_air_mouse_reset_motion(mouse);
        return true;
    }

    if (!(delta_seconds > 0.0f) || !isfinite(delta_seconds)) {
        delta_seconds = AIR_MOUSE_DEFAULT_DELTA_SECONDS;
    } else if (delta_seconds > AIR_MOUSE_MAX_DELTA_SECONDS) {
        delta_seconds = AIR_MOUSE_MAX_DELTA_SECONDS;
    }

    float horizontal =
        sample->gyro_dps[mouse->config.horizontal_axis] -
        mouse->gyro_bias[mouse->config.horizontal_axis];
    float vertical = sample->gyro_dps[mouse->config.vertical_axis] -
                     mouse->gyro_bias[mouse->config.vertical_axis];
    float wheel = sample->gyro_dps[mouse->config.wheel_axis] -
                  mouse->gyro_bias[mouse->config.wheel_axis];
    horizontal = apply_deadzone(horizontal, mouse->config.pointer_deadzone_dps) *
                 (float)mouse->config.horizontal_sign *
                 mouse->config.horizontal_gain;
    vertical = apply_deadzone(vertical, mouse->config.pointer_deadzone_dps) *
               (float)mouse->config.vertical_sign *
               mouse->config.vertical_gain;
    wheel = apply_deadzone(wheel, mouse->config.wheel_deadzone_dps) *
            (float)mouse->config.wheel_sign;

    mouse->filtered_horizontal_dps +=
        AIR_MOUSE_FILTER_ALPHA *
        (horizontal - mouse->filtered_horizontal_dps);
    mouse->filtered_vertical_dps +=
        AIR_MOUSE_FILTER_ALPHA *
        (vertical - mouse->filtered_vertical_dps);
    output->dx = consume_pointer_axis(
        pointer_speed(mouse->filtered_horizontal_dps) * delta_seconds,
        &mouse->horizontal_remainder);
    output->dy = consume_pointer_axis(
        pointer_speed(mouse->filtered_vertical_dps) * delta_seconds,
        &mouse->vertical_remainder);
    output->wheel = consume_wheel(mouse, wheel, delta_seconds);
    return true;
}

bool vibe_air_mouse_calibrated(const vibe_air_mouse_t *mouse)
{
    return mouse && mouse->calibrated;
}

uint16_t vibe_air_mouse_calibration_progress(const vibe_air_mouse_t *mouse)
{
    if (!mouse) {
        return 0;
    }
    return mouse->calibration_samples > VIBE_AIR_MOUSE_CALIBRATION_SAMPLES
               ? VIBE_AIR_MOUSE_CALIBRATION_SAMPLES
               : mouse->calibration_samples;
}
