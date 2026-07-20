#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*vibe_input_callback_t)(void *button_handle, void *context);

typedef struct {
    vibe_input_callback_t front_down;
    vibe_input_callback_t front_single;
    vibe_input_callback_t front_double;
    vibe_input_callback_t front_long;
    vibe_input_callback_t front_confirm;
    vibe_input_callback_t front_up;
    vibe_input_callback_t side_up;
    vibe_input_callback_t side_mode_hold;
    vibe_input_callback_t side_calibration_hold;
    void *context;
} vibe_input_callbacks_t;

typedef struct {
    uint32_t front_long_ms;
    uint32_t front_confirm_ms;
    uint32_t side_mode_ms;
    uint32_t side_calibration_ms;
} vibe_input_config_t;

esp_err_t vibe_input_init(const vibe_input_config_t *config,
                          const vibe_input_callbacks_t *callbacks);
bool vibe_input_front_pressed(void);
