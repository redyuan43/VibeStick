#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VIBE_MOTION_EVENT_NONE,
    VIBE_MOTION_EVENT_LIFTED,
    VIBE_MOTION_EVENT_FLAT,
} vibe_motion_event_t;

esp_err_t vibe_motion_init(void);
bool vibe_motion_available(void);
esp_err_t vibe_motion_recalibrate(void);
bool vibe_motion_is_calibrating(void);
vibe_motion_event_t vibe_motion_poll(int64_t now_ms);
