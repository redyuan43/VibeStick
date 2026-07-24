#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VIBE_MOTION_EVENT_NONE,
    VIBE_MOTION_EVENT_LIFTED,
    VIBE_MOTION_EVENT_FLAT,
} vibe_motion_event_t;

typedef struct {
    float baseline[3];
    float gyro_bias[3];
} vibe_motion_calibration_t;

typedef struct {
    float accel_g[3];
    float gyro_dps[3];
} vibe_motion_sample_t;

esp_err_t vibe_motion_init(void);
bool vibe_motion_available(void);
esp_err_t vibe_motion_read_raw_sample(vibe_motion_sample_t *sample);
esp_err_t vibe_motion_suspend(void);
esp_err_t vibe_motion_resume(void);
bool vibe_motion_suspended(void);
esp_err_t vibe_motion_recalibrate(void);
esp_err_t vibe_motion_get_calibration(vibe_motion_calibration_t *calibration);
esp_err_t vibe_motion_apply_calibration(const vibe_motion_calibration_t *calibration);
bool vibe_motion_calibration_valid(const vibe_motion_calibration_t *calibration);
esp_err_t vibe_motion_clear_wake_status(void);
esp_err_t vibe_motion_prepare_deep_sleep(void);
esp_err_t vibe_motion_prepare_deep_sleep_wake(void);
bool vibe_motion_is_calibrating(void);
bool vibe_motion_is_lifted(void);
vibe_motion_event_t vibe_motion_poll(int64_t now_ms);
