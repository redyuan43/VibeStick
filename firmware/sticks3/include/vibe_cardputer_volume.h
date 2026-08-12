#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "vibe_keyboard.h"

typedef struct {
    void (*display_lock)(void);
    void (*display_unlock)(void);
    void (*activity)(void);
} vibe_cardputer_volume_config_t;

esp_err_t vibe_cardputer_volume_init(
    const vibe_cardputer_volume_config_t *config);
bool vibe_cardputer_volume_handle_key(const vibe_key_event_t *event);
