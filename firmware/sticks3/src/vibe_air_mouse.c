#include "vibe_air_mouse.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define AIR_MOUSE_CALIBRATION_SAMPLES 50
#define AIR_MOUSE_STILL_ACCEL_TOLERANCE_G 0.15f
#define AIR_MOUSE_STILL_ACCEL_DELTA_G 0.05f
#define AIR_MOUSE_STILL_GYRO_DELTA_DPS 5.0f
#define AIR_MOUSE_MAX_GYRO_BIAS_DPS 120.0f
#define AIR_MOUSE_DEADZONE_DPS 2.5f
#define AIR_MOUSE_FILTER_ALPHA 0.45f
#define AIR_MOUSE_LINEAR_PIXELS_PER_DEGREE 8.0f
#define AIR_MOUSE_ACCEL_PIXELS_PER_DEGREE_SQUARED 0.015f
#define AIR_MOUSE_DEFAULT_DELTA_SECONDS 0.02f
#define AIR_MOUSE_MAX_DELTA_SECONDS 0.05f
#define AIR_MOUSE_PITCH_FILTER_ALPHA 0.20f
#define AIR_MOUSE_TILT_TRIGGER_DEGREES 22.0f
#define AIR_MOUSE_TILT_NEUTRAL_DEGREES 8.0f
#define AIR_MOUSE_TILT_TRIGGER_SECONDS 0.10f
#define AIR_MOUSE_TILT_NEUTRAL_SECONDS 0.20f
#define AIR_MOUSE_TILT_REPEAT_SECONDS 0.12f
#define AIR_MOUSE_MOTION_TRIGGER_DPS 18.0f
#define AIR_MOUSE_MOTION_QUIET_DPS 5.0f
#define AIR_MOUSE_MOTION_TRIGGER_SECONDS 0.08f
#define AIR_MOUSE_MOTION_REARM_SECONDS 1.0f
#define AIR_MOUSE_RADIANS_TO_DEGREES 57.2957795f

static float vector_norm(const float vector[3])
{
    return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] +
                 vector[2] * vector[2]);
}

static bool sample_is_stationary(const vibe_air_mouse_t *mouse,
                                 const vibe_air_mouse_sample_t *sample)
{
    if (fabsf(vector_norm(sample->accel_g) - 1.0f) >
            AIR_MOUSE_STILL_ACCEL_TOLERANCE_G ||
        vector_norm(sample->gyro_dps) > AIR_MOUSE_MAX_GYRO_BIAS_DPS) {
        return false;
    }
    if (!mouse->has_previous_calibration_sample) {
        return true;
    }

    float accel_delta[3];
    float gyro_delta[3];
    for (size_t axis = 0; axis < 3; ++axis) {
        accel_delta[axis] = sample->accel_g[axis] -
                            mouse->previous_calibration_accel_g[axis];
        gyro_delta[axis] = sample->gyro_dps[axis] -
                           mouse->previous_calibration_gyro_dps[axis];
    }
    return vector_norm(accel_delta) <= AIR_MOUSE_STILL_ACCEL_DELTA_G &&
           vector_norm(gyro_delta) <= AIR_MOUSE_STILL_GYRO_DELTA_DPS;
}

static void remember_calibration_sample(vibe_air_mouse_t *mouse,
                                        const vibe_air_mouse_sample_t *sample)
{
    memcpy(mouse->previous_calibration_accel_g, sample->accel_g,
           sizeof(mouse->previous_calibration_accel_g));
    memcpy(mouse->previous_calibration_gyro_dps, sample->gyro_dps,
           sizeof(mouse->previous_calibration_gyro_dps));
    mouse->has_previous_calibration_sample = true;
}

static float apply_deadzone(float value)
{
    float magnitude = fabsf(value);
    if (magnitude <= AIR_MOUSE_DEADZONE_DPS) {
        return 0.0f;
    }
    magnitude -= AIR_MOUSE_DEADZONE_DPS;
    return copysignf(magnitude, value);
}

static float pointer_speed(float angular_rate_dps)
{
    return AIR_MOUSE_LINEAR_PIXELS_PER_DEGREE * angular_rate_dps +
           AIR_MOUSE_ACCEL_PIXELS_PER_DEGREE_SQUARED * angular_rate_dps *
               fabsf(angular_rate_dps);
}

static int8_t consume_axis(float delta, float *remainder)
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

static float normalize_angle_degrees(float angle)
{
    while (angle > 180.0f) {
        angle -= 360.0f;
    }
    while (angle < -180.0f) {
        angle += 360.0f;
    }
    return angle;
}

static void update_tilt_gesture(vibe_air_mouse_t *mouse,
                                const vibe_air_mouse_sample_t *sample,
                                float delta_seconds,
                                vibe_air_mouse_output_t *output)
{
    float baseline_pitch =
        atan2f(-mouse->accel_baseline[0], mouse->accel_baseline[2]) *
        AIR_MOUSE_RADIANS_TO_DEGREES;
    float current_pitch =
        atan2f(-sample->accel_g[0], sample->accel_g[2]) *
        AIR_MOUSE_RADIANS_TO_DEGREES;
    float pitch_delta =
        normalize_angle_degrees(current_pitch - baseline_pitch);
    mouse->filtered_pitch_degrees +=
        AIR_MOUSE_PITCH_FILTER_ALPHA *
        (pitch_delta - mouse->filtered_pitch_degrees);

    if (!mouse->tilt_armed) {
        if (mouse->tilt_active_direction != 0) {
            if (fabsf(pitch_delta) <= AIR_MOUSE_TILT_NEUTRAL_DEGREES) {
                mouse->tilt_active_direction = 0;
                mouse->tilt_repeat_seconds = 0.0f;
                mouse->tilt_neutral_seconds = delta_seconds;
                return;
            }
            mouse->tilt_repeat_seconds += delta_seconds;
            if (mouse->tilt_repeat_seconds >=
                AIR_MOUSE_TILT_REPEAT_SECONDS) {
                output->wheel = mouse->tilt_active_direction;
                mouse->tilt_repeat_seconds -=
                    AIR_MOUSE_TILT_REPEAT_SECONDS;
            }
            return;
        }
        if (fabsf(pitch_delta) <= AIR_MOUSE_TILT_NEUTRAL_DEGREES) {
            mouse->tilt_neutral_seconds += delta_seconds;
            if (mouse->tilt_neutral_seconds >=
                AIR_MOUSE_TILT_NEUTRAL_SECONDS) {
                mouse->tilt_armed = true;
                mouse->tilt_neutral_seconds = 0.0f;
            }
        } else {
            mouse->tilt_neutral_seconds = 0.0f;
        }
        return;
    }

    int8_t direction = 0;
    if (mouse->filtered_pitch_degrees >= AIR_MOUSE_TILT_TRIGGER_DEGREES) {
        direction = 1;
    } else if (mouse->filtered_pitch_degrees <=
               -AIR_MOUSE_TILT_TRIGGER_DEGREES) {
        direction = -1;
    }
    if (direction == 0) {
        mouse->tilt_candidate_direction = 0;
        mouse->tilt_candidate_seconds = 0.0f;
        return;
    }
    if (direction != mouse->tilt_candidate_direction) {
        mouse->tilt_candidate_direction = direction;
        mouse->tilt_candidate_seconds = 0.0f;
    }
    mouse->tilt_candidate_seconds += delta_seconds;
    if (mouse->tilt_candidate_seconds < AIR_MOUSE_TILT_TRIGGER_SECONDS) {
        return;
    }

    output->wheel = direction;
    mouse->tilt_armed = false;
    mouse->tilt_active_direction = direction;
    mouse->tilt_candidate_direction = 0;
    mouse->tilt_candidate_seconds = 0.0f;
    mouse->tilt_neutral_seconds = 0.0f;
    mouse->tilt_repeat_seconds = 0.0f;
}

static void update_motion_sound(vibe_air_mouse_t *mouse,
                                const vibe_air_mouse_sample_t *sample,
                                float delta_seconds,
                                vibe_air_mouse_output_t *output)
{
    float max_rate = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        float rate = fabsf(sample->gyro_dps[axis] - mouse->gyro_bias[axis]);
        if (rate > max_rate) {
            max_rate = rate;
        }
    }

    if (mouse->motion_sound_armed) {
        if (max_rate >= AIR_MOUSE_MOTION_TRIGGER_DPS) {
            mouse->motion_candidate_seconds += delta_seconds;
            if (mouse->motion_candidate_seconds >=
                AIR_MOUSE_MOTION_TRIGGER_SECONDS) {
                output->motion_started = true;
                mouse->motion_sound_armed = false;
                mouse->motion_candidate_seconds = 0.0f;
                mouse->motion_quiet_seconds = 0.0f;
            }
        } else {
            mouse->motion_candidate_seconds = 0.0f;
        }
        return;
    }

    if (max_rate <= AIR_MOUSE_MOTION_QUIET_DPS) {
        mouse->motion_quiet_seconds += delta_seconds;
        if (mouse->motion_quiet_seconds >= AIR_MOUSE_MOTION_REARM_SECONDS) {
            mouse->motion_sound_armed = true;
            mouse->motion_quiet_seconds = 0.0f;
        }
    } else {
        mouse->motion_quiet_seconds = 0.0f;
    }
}

void vibe_air_mouse_init(vibe_air_mouse_t *mouse,
                         const vibe_air_mouse_config_t *config)
{
    if (!mouse || !config) {
        return;
    }
    memset(mouse, 0, sizeof(*mouse));
    mouse->config = *config;
    if (!(mouse->config.horizontal_gain > 0.0f) ||
        !isfinite(mouse->config.horizontal_gain)) {
        mouse->config.horizontal_gain = 1.0f;
    }
    if (!(mouse->config.vertical_gain > 0.0f) ||
        !isfinite(mouse->config.vertical_gain)) {
        mouse->config.vertical_gain = 1.0f;
    }
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
    mouse->filtered_pitch_degrees = 0.0f;
    mouse->tilt_candidate_seconds = 0.0f;
    mouse->tilt_neutral_seconds = 0.0f;
    mouse->tilt_repeat_seconds = 0.0f;
    mouse->motion_candidate_seconds = 0.0f;
    mouse->motion_quiet_seconds = 0.0f;
    mouse->tilt_candidate_direction = 0;
    mouse->tilt_active_direction = 0;
    mouse->tilt_armed = mouse->calibrated;
    mouse->motion_sound_armed = mouse->calibrated;
}

bool vibe_air_mouse_update(vibe_air_mouse_t *mouse,
                           const vibe_air_mouse_sample_t *sample,
                           float delta_seconds,
                           vibe_air_mouse_output_t *output)
{
    if (!mouse || !sample || !output ||
        mouse->config.horizontal_axis > 2 || mouse->config.vertical_axis > 2) {
        return false;
    }
    *output = (vibe_air_mouse_output_t){0};

    if (!mouse->calibrated) {
        if (!sample_is_stationary(mouse, sample)) {
            memset(mouse->accel_baseline_sum, 0,
                   sizeof(mouse->accel_baseline_sum));
            memset(mouse->gyro_bias_sum, 0, sizeof(mouse->gyro_bias_sum));
            mouse->calibration_samples = 0;
            mouse->has_previous_calibration_sample = false;
            return false;
        }
        remember_calibration_sample(mouse, sample);
        for (size_t axis = 0; axis < 3; ++axis) {
            mouse->accel_baseline_sum[axis] += sample->accel_g[axis];
            mouse->gyro_bias_sum[axis] += sample->gyro_dps[axis];
        }
        ++mouse->calibration_samples;
        if (mouse->calibration_samples < AIR_MOUSE_CALIBRATION_SAMPLES) {
            return false;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            mouse->accel_baseline[axis] =
                mouse->accel_baseline_sum[axis] /
                (float)mouse->calibration_samples;
            mouse->gyro_bias[axis] =
                mouse->gyro_bias_sum[axis] / (float)mouse->calibration_samples;
        }
        mouse->calibrated = true;
        vibe_air_mouse_reset_motion(mouse);
        return true;
    }

    if (!(delta_seconds > 0.0f) || !isfinite(delta_seconds)) {
        delta_seconds = AIR_MOUSE_DEFAULT_DELTA_SECONDS;
    } else if (delta_seconds > AIR_MOUSE_MAX_DELTA_SECONDS) {
        delta_seconds = AIR_MOUSE_MAX_DELTA_SECONDS;
    }

    float horizontal = sample->gyro_dps[mouse->config.horizontal_axis] -
                       mouse->gyro_bias[mouse->config.horizontal_axis];
    float vertical = sample->gyro_dps[mouse->config.vertical_axis] -
                     mouse->gyro_bias[mouse->config.vertical_axis];
    horizontal = apply_deadzone(horizontal) *
                 (float)mouse->config.horizontal_sign *
                 mouse->config.horizontal_gain;
    vertical = apply_deadzone(vertical) *
               (float)mouse->config.vertical_sign *
               mouse->config.vertical_gain;
    mouse->filtered_horizontal_dps +=
        AIR_MOUSE_FILTER_ALPHA *
        (horizontal - mouse->filtered_horizontal_dps);
    mouse->filtered_vertical_dps +=
        AIR_MOUSE_FILTER_ALPHA *
        (vertical - mouse->filtered_vertical_dps);

    output->dx = consume_axis(pointer_speed(mouse->filtered_horizontal_dps) *
                                  delta_seconds,
                              &mouse->horizontal_remainder);
    output->dy = consume_axis(pointer_speed(mouse->filtered_vertical_dps) *
                                  delta_seconds,
                              &mouse->vertical_remainder);
    update_tilt_gesture(mouse, sample, delta_seconds, output);
    update_motion_sound(mouse, sample, delta_seconds, output);
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
    return mouse->calibration_samples > AIR_MOUSE_CALIBRATION_SAMPLES
               ? AIR_MOUSE_CALIBRATION_SAMPLES
               : mouse->calibration_samples;
}
