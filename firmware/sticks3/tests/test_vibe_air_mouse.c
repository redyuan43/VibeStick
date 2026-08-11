#include "vibe_air_mouse.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static vibe_air_mouse_sample_t stationary_sample(void)
{
    return (vibe_air_mouse_sample_t){
        .accel_g = {0.01f, 0.02f, 1.00f},
        .gyro_dps = {1.0f, -2.0f, 3.0f},
    };
}

static void calibrate(vibe_air_mouse_t *mouse)
{
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    for (int index = 0; index < VIBE_AIR_MOUSE_CALIBRATION_SAMPLES;
         ++index) {
        vibe_air_mouse_update(mouse, &sample, 0.02f, &output);
    }
    assert(vibe_air_mouse_calibrated(mouse));
}

static void test_calibration_requires_ten_seconds_of_stillness(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    for (int index = 0; index < 250; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    }
    assert(!vibe_air_mouse_calibrated(&mouse));
    assert(vibe_air_mouse_calibration_progress(&mouse) == 250);
    sample.gyro_dps[2] += 20.0f;
    vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
    assert(vibe_air_mouse_calibration_progress(&mouse) == 0);
    calibrate(&mouse);
}

static void test_cardputer_pointer_axis_mapping(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    calibrate(&mouse);
    vibe_air_mouse_output_t output = {0};
    vibe_air_mouse_sample_t sample = stationary_sample();

    sample.gyro_dps[2] -= 30.0f;
    bool moved_right = false;
    for (int index = 0; index < 8; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        moved_right |= output.dx > 0;
        assert(output.dy == 0);
        assert(output.wheel == 0);
    }
    assert(moved_right);

    vibe_air_mouse_reset_motion(&mouse);
    sample = stationary_sample();
    sample.gyro_dps[0] -= 30.0f;
    bool moved_up = false;
    for (int index = 0; index < 8; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        moved_up |= output.dy < 0;
        assert(output.dx == 0);
        assert(output.wheel == 0);
    }
    assert(moved_up);
}

static void test_cardputer_wheel_axis_mapping(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    calibrate(&mouse);
    vibe_air_mouse_output_t output = {0};
    vibe_air_mouse_sample_t sample = stationary_sample();

    sample.gyro_dps[1] += 45.0f;
    bool positive_y_scrolls_down = false;
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        positive_y_scrolls_down |= output.wheel < 0;
        assert(output.dx == 0);
        assert(output.dy == 0);
        assert(output.wheel <= 0);
    }
    assert(positive_y_scrolls_down);

    sample = stationary_sample();
    sample.gyro_dps[1] -= 45.0f;
    bool negative_y_scrolls_up = false;
    for (int index = 0; index < 20; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        negative_y_scrolls_up |= output.wheel > 0;
        assert(output.wheel >= 0);
    }
    assert(negative_y_scrolls_up);
}

static void test_deadzone_and_axis_isolation(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    calibrate(&mouse);
    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    sample.gyro_dps[0] += 2.0f;
    sample.gyro_dps[1] -= 4.0f;
    sample.gyro_dps[2] += 2.0f;
    for (int index = 0; index < 100; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        assert(output.dx == 0);
        assert(output.dy == 0);
        assert(output.wheel == 0);
    }
}

static void test_calibration_round_trip_and_validation(void)
{
    vibe_air_mouse_t source;
    vibe_air_mouse_init(&source, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    calibrate(&source);
    vibe_air_mouse_calibration_t calibration = {0};
    assert(vibe_air_mouse_get_calibration(&source, &calibration));

    vibe_air_mouse_t restored;
    vibe_air_mouse_init(&restored,
                        &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    assert(vibe_air_mouse_set_calibration(&restored, &calibration));
    assert(vibe_air_mouse_calibrated(&restored));

    calibration.gyro_bias[1] = NAN;
    assert(!vibe_air_mouse_set_calibration(&restored, &calibration));
}

static void test_runtime_config_changes_direction_and_deadzone(void)
{
    vibe_air_mouse_t mouse;
    vibe_air_mouse_init(&mouse, &VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG);
    calibrate(&mouse);
    vibe_air_mouse_config_t config =
        VIBE_AIR_MOUSE_CARDPUTER_FACE_UP_CONFIG;
    config.horizontal_sign *= -1;
    config.pointer_deadzone_dps = 6.0f;
    assert(vibe_air_mouse_set_config(&mouse, &config));

    vibe_air_mouse_sample_t sample = stationary_sample();
    vibe_air_mouse_output_t output = {0};
    sample.gyro_dps[2] -= 30.0f;
    bool moved_left = false;
    for (int index = 0; index < 8; ++index) {
        vibe_air_mouse_update(&mouse, &sample, 0.02f, &output);
        moved_left |= output.dx < 0;
    }
    assert(moved_left);
}

int main(void)
{
    test_calibration_requires_ten_seconds_of_stillness();
    test_cardputer_pointer_axis_mapping();
    test_cardputer_wheel_axis_mapping();
    test_deadzone_and_axis_isolation();
    test_calibration_round_trip_and_validation();
    test_runtime_config_changes_direction_and_deadzone();
    puts("vibe_air_mouse tests passed");
    return 0;
}
