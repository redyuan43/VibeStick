#include "vibe_air_mouse.h"

#include <assert.h>
#include <stdio.h>

static const vibe_air_mouse_config_t k_config = {
    .horizontal_axis = 2,
    .horizontal_sign = -1,
    .horizontal_gain = 1.8f,
    .vertical_axis = 0,
    .vertical_sign = -1,
    .vertical_gain = 1.0f,
};

static vibe_air_mouse_sample_t stationary_sample(void)
{
    return (vibe_air_mouse_sample_t){
        .accel_g = {0.018f, 0.018f, 1.085f},
        .gyro_dps = {-5.0f, 28.0f, 8.0f},
    };
}

static void calibrate(vibe_air_mouse_t *mouse)
{
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    for (int index = 0; index < 50; ++index) {
        vibe_air_mouse_update(mouse, &sample, 0.02f, &output);
    }
    assert(vibe_air_mouse_calibrated(mouse));
}

static void test_calibration_requires_stillness(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    }
    assert(vibe_air_mouse_calibration_progress(&mouse) == 20);
    sample.gyro_dps[2] += 20.0f;
    vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    assert(vibe_air_mouse_calibration_progress(&mouse) == 0);
    calibrate(&mouse);
}

static void test_calibration_rejects_implausible_bias(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    vibe_air_mouse_sample_t sample = stationary_sample();
    sample.gyro_dps[1] = 150.0f;
    vibe_air_mouse_output_t output = {0};
    for (int index = 0; index < 60; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    }
    assert(!vibe_air_mouse_calibrated(&mouse));
    assert(vibe_air_mouse_calibration_progress(&mouse) == 0);
}

static void test_axis_mapping_deadzone_and_direction(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    calibrate(&mouse);
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};

    sample.gyro_dps[2] -= 2.0f;
    for (int index = 0; index < 10; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        assert(output.dx == 0);
        assert(output.dy == 0);
    }

    sample = stationary_sample();
    sample.gyro_dps[2] -= 30.0f;
    bool moved_right = false;
    for (int index = 0; index < 8; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        moved_right |= output.dx > 0;
        assert(output.dy == 0);
    }
    assert(moved_right);

    vibe_air_mouse_reset_motion(&mouse);
    sample = stationary_sample();
    sample.gyro_dps[0] += 30.0f;
    bool moved_up = false;
    for (int index = 0; index < 8; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        moved_up |= output.dy < 0;
        assert(output.dx == 0);
    }
    assert(moved_up);

    vibe_air_mouse_reset_motion(&mouse);
    sample = stationary_sample();
    sample.gyro_dps[1] += 40.0f;
    for (int index = 0; index < 12; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        assert(output.dx == 0);
        assert(output.dy == 0);
        assert(output.wheel == 0);
    }
}

static void test_fractional_motion_and_clamp(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    calibrate(&mouse);
    vibe_air_mouse_sample_t sample = stationary_sample();
    sample.gyro_dps[2] -= 3.0f;
    vibe_air_mouse_output_t output = {0};
    bool eventually_moved = false;
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        eventually_moved |= output.dx != 0;
    }
    assert(eventually_moved);

    sample.gyro_dps[2] = -5000.0f;
    vibe_air_mouse_update(&mouse, &sample, 0.05f, &output);
    assert(output.dx == 127);
}

static void test_scroll_gesture_requires_neutral_rearm(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    calibrate(&mouse);
    vibe_air_mouse_output_t output = {0};
    vibe_air_mouse_sample_t forward = stationary_sample();
    forward.accel_g[0] = -0.5f;
    forward.accel_g[1] = 0.0f;
    forward.accel_g[2] = 0.866f;

    int wheel_up_count = 0;
    for (int index = 0; index < 60; ++index) {
        vibe_air_mouse_update(&mouse, &forward, 0.02f, &output);
        wheel_up_count += output.wheel > 0;
        assert(output.wheel >= 0);
    }
    assert(wheel_up_count >= 5);

    vibe_air_mouse_sample_t neutral = stationary_sample();
    for (int index = 0; index < 50; ++index) {
        vibe_air_mouse_update(&mouse, &neutral, 0.02f, &output);
        assert(output.wheel == 0);
    }

    vibe_air_mouse_sample_t backward = stationary_sample();
    backward.accel_g[0] = 0.5f;
    backward.accel_g[1] = 0.0f;
    backward.accel_g[2] = 0.866f;
    int wheel_down_count = 0;
    for (int index = 0; index < 60; ++index) {
        vibe_air_mouse_update(&mouse, &backward, 0.02f, &output);
        wheel_down_count += output.wheel < 0;
        assert(output.wheel <= 0);
    }
    assert(wheel_down_count >= 5);
}

static void test_motion_sound_rejects_noise_and_rearms(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    calibrate(&mouse);
    vibe_air_mouse_output_t output = {0};
    vibe_air_mouse_sample_t sample = stationary_sample();

    sample.gyro_dps[0] += 2.0f;
    for (int index = 0; index < 100; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        assert(!output.motion_started);
        assert(output.dx == 0);
        assert(output.dy == 0);
    }

    sample = stationary_sample();
    sample.gyro_dps[1] += 20.0f;
    int sound_count = 0;
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        sound_count += output.motion_started;
    }
    assert(sound_count == 1);

    sample = stationary_sample();
    for (int index = 0; index < 60; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        assert(!output.motion_started);
    }
    sample.gyro_dps[1] += 20.0f;
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        sound_count += output.motion_started;
    }
    assert(sound_count == 2);
}

static void test_motion_reset_preserves_calibration(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &k_config);
    calibrate(&mouse);

    vibe_air_mouse_sample_t sample = stationary_sample();
    sample.gyro_dps[2] -= 30.0f;
    vibe_air_mouse_output_t output = {0};
    vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    vibe_air_mouse_reset_motion(&mouse);

    assert(vibe_air_mouse_calibrated(&mouse));
    sample = stationary_sample();
    vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    assert(output.dx == 0);
    assert(output.dy == 0);
    assert(output.wheel == 0);
    assert(!output.motion_started);
}

int main(void)
{
    test_calibration_requires_stillness();
    test_calibration_rejects_implausible_bias();
    test_axis_mapping_deadzone_and_direction();
    test_fractional_motion_and_clamp();
    test_scroll_gesture_requires_neutral_rearm();
    test_motion_sound_rejects_noise_and_rearms();
    test_motion_reset_preserves_calibration();
    puts("vibe_air_mouse tests passed");
    return 0;
}
