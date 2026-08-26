#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    VIBE_CARDPUTER_STATUS_WIFI,
    VIBE_CARDPUTER_STATUS_READY,
    VIBE_CARDPUTER_STATUS_RECORDING,
    VIBE_CARDPUTER_STATUS_SENDING,
    VIBE_CARDPUTER_STATUS_DONE,
    VIBE_CARDPUTER_STATUS_ERROR,
} vibe_cardputer_status_t;

esp_err_t vibe_cardputer_status_init(void);
void vibe_cardputer_status_show(vibe_cardputer_status_t status);
void vibe_cardputer_status_set_battery_level(int level_percent);
void vibe_cardputer_status_set_display_enabled(bool enabled);
void vibe_cardputer_status_set_recording_animation(bool enabled);
