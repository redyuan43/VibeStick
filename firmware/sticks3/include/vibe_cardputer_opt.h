#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VIBE_CARDPUTER_KEY_NONE,
    VIBE_CARDPUTER_KEY_BACKSPACE,
    VIBE_CARDPUTER_KEY_TAB,
    VIBE_CARDPUTER_KEY_FN,
    VIBE_CARDPUTER_KEY_SHIFT,
    VIBE_CARDPUTER_KEY_ENTER,
    VIBE_CARDPUTER_KEY_CTRL,
    VIBE_CARDPUTER_KEY_OPT,
    VIBE_CARDPUTER_KEY_ALT,
    VIBE_CARDPUTER_KEY_ESCAPE,
    VIBE_CARDPUTER_KEY_DELETE,
    VIBE_CARDPUTER_KEY_UP,
    VIBE_CARDPUTER_KEY_LEFT,
    VIBE_CARDPUTER_KEY_DOWN,
    VIBE_CARDPUTER_KEY_RIGHT,
} vibe_cardputer_key_t;

typedef struct {
    bool pressed;
    uint8_t row;
    uint8_t column;
    uint8_t hid_usage;
    uint8_t hid_modifiers;
    vibe_cardputer_key_t key;
} vibe_cardputer_key_event_t;

typedef void (*vibe_cardputer_key_callback_t)(
    const vibe_cardputer_key_event_t *event, void *context);

esp_err_t vibe_cardputer_opt_init(vibe_cardputer_key_callback_t callback,
                                  void *context);
