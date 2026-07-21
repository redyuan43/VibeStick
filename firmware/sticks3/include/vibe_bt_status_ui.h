#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VIBE_BT_UI_WAITING,
    VIBE_BT_UI_PAIRING,
    VIBE_BT_UI_CONNECTED,
    VIBE_BT_UI_RECORDING,
    VIBE_BT_UI_ERROR,
} vibe_bt_ui_status_t;

esp_err_t vibe_bt_status_ui_init(void);
void vibe_bt_status_ui_set(vibe_bt_ui_status_t status, bool minijoy_ready);
void vibe_bt_status_ui_set_confirm_window(bool active);
void vibe_bt_status_ui_activity(void);
void vibe_bt_status_ui_tick(int64_t now_ms, uint8_t audio_level);
