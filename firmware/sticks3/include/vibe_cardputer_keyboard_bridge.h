#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "vibe_cardputer_opt.h"

typedef esp_err_t (*vibe_cardputer_keyboard_post_fn)(const char *body,
                                                      void *context);
typedef bool (*vibe_cardputer_keyboard_online_fn)(void *context);

typedef struct {
    vibe_cardputer_keyboard_post_fn post;
    vibe_cardputer_keyboard_online_fn online;
    void *context;
} vibe_cardputer_keyboard_bridge_config_t;

esp_err_t vibe_cardputer_keyboard_bridge_init(
    const vibe_cardputer_keyboard_bridge_config_t *config);
void vibe_cardputer_keyboard_bridge_handle(
    const vibe_cardputer_key_event_t *event);
void vibe_cardputer_keyboard_bridge_suspend(void);
void vibe_cardputer_keyboard_bridge_resume(void);
