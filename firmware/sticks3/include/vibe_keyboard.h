#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    VIBE_KEY_NONE = 0,
    VIBE_KEY_TAB,
    VIBE_KEY_BACKSPACE,
    VIBE_KEY_ENTER,
    VIBE_KEY_ESCAPE,
    VIBE_KEY_DELETE,
    VIBE_KEY_UP,
    VIBE_KEY_DOWN,
    VIBE_KEY_LEFT,
    VIBE_KEY_RIGHT,
    VIBE_KEY_FN,
    VIBE_KEY_SHIFT,
    VIBE_KEY_CTRL,
    VIBE_KEY_ALT,
    VIBE_KEY_OPT,
} vibe_key_code_t;

typedef struct {
    bool pressed;
    uint8_t row;
    uint8_t column;
    char character;
    vibe_key_code_t key;
    uint8_t hid_usage;
    uint8_t hid_modifiers;
    bool fn;
    bool shift;
    bool ctrl;
    bool alt;
    bool opt;
} vibe_key_event_t;

typedef void (*vibe_keyboard_callback_t)(const vibe_key_event_t *event,
                                         void *context);

esp_err_t vibe_keyboard_init(vibe_keyboard_callback_t callback, void *context);
