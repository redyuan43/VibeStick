#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    VIBE_STICKS3_STATUS_WIFI,
    VIBE_STICKS3_STATUS_READY,
    VIBE_STICKS3_STATUS_RECORDING,
    VIBE_STICKS3_STATUS_SENDING,
    VIBE_STICKS3_STATUS_DONE,
    VIBE_STICKS3_STATUS_ERROR,
} vibe_sticks3_status_t;

esp_err_t vibe_sticks3_status_init(void);
void vibe_sticks3_status_show(vibe_sticks3_status_t status);
void vibe_sticks3_status_set_battery_level(int level_percent);
void vibe_sticks3_status_set_recording_animation(bool enabled);
