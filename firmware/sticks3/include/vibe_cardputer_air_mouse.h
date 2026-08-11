#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "vibe_air_mouse.h"
#include "vibe_keyboard.h"
#include "vibe_pointer_client.h"

typedef enum {
    VIBE_CARD_AIR_MOUSE_DISABLED,
    VIBE_CARD_AIR_MOUSE_CALIBRATING,
    VIBE_CARD_AIR_MOUSE_READY,
    VIBE_CARD_AIR_MOUSE_BUSY,
    VIBE_CARD_AIR_MOUSE_UNAVAILABLE,
    VIBE_CARD_AIR_MOUSE_FAILED,
} vibe_card_air_mouse_status_t;

typedef bool (*vibe_card_air_mouse_bool_fn)(void *context);
typedef void (*vibe_card_air_mouse_void_fn)(void *context);
typedef void (*vibe_card_air_mouse_status_fn)(
    vibe_card_air_mouse_status_t status, uint16_t progress, void *context);

typedef struct {
    vibe_pointer_client_config_t pointer;
    vibe_card_air_mouse_bool_fn busy;
    vibe_card_air_mouse_bool_fn keep_motion_active;
    vibe_card_air_mouse_void_fn release_keyboard;
    vibe_card_air_mouse_void_fn activity;
    vibe_card_air_mouse_status_fn status;
    void *context;
} vibe_card_air_mouse_config_t;

typedef struct {
    bool invert_horizontal;
    bool invert_vertical;
    bool invert_scroll;
    float pointer_speed;
    float wheel_speed;
    float pointer_deadzone_dps;
    float wheel_deadzone_dps;
} vibe_card_air_mouse_settings_t;

esp_err_t vibe_cardputer_air_mouse_init(
    const vibe_card_air_mouse_config_t *config);
bool vibe_cardputer_air_mouse_handle_key(const vibe_key_event_t *event);
void vibe_cardputer_air_mouse_poll(int64_t now_ms);
void vibe_cardputer_air_mouse_stop(void);
bool vibe_cardputer_air_mouse_enabled(void);
bool vibe_cardputer_air_mouse_apply_settings(
    const vibe_card_air_mouse_settings_t *settings);
