#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int16_t x;
    int16_t y;
    bool button_pressed;
} vibe_minijoyc_state_t;

esp_err_t vibe_minijoyc_open(void);
void vibe_minijoyc_close(void);
esp_err_t vibe_minijoyc_suspend_for_microphone(void);
bool vibe_minijoyc_available(void);
esp_err_t vibe_minijoyc_read(vibe_minijoyc_state_t *state);
esp_err_t vibe_minijoyc_set_led(uint32_t rgb);
