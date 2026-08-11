#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "vibe_cardputer_air_mouse.h"

typedef enum {
    VIBE_CARD_ROUTE_NONE = 0,
    VIBE_CARD_ROUTE_HOST,
    VIBE_CARD_ROUTE_DEVICE_RECORDING_TOGGLE,
    VIBE_CARD_ROUTE_DEVICE_RECORDING_HOLD,
    VIBE_CARD_ROUTE_DEVICE_LEGACY_DOUBLE,
} vibe_card_input_route_t;

typedef struct {
    uint32_t revision;
    vibe_card_input_route_t opt_tap;
    vibe_card_input_route_t opt_double;
    vibe_card_input_route_t opt_hold;
    vibe_card_air_mouse_settings_t air_mouse;
} vibe_card_input_profile_t;

void vibe_card_input_profile_default(vibe_card_input_profile_t *profile);
bool vibe_card_input_profile_valid(const vibe_card_input_profile_t *profile);
esp_err_t vibe_card_input_profile_load(vibe_card_input_profile_t *profile);
esp_err_t vibe_card_input_profile_save(const vibe_card_input_profile_t *profile);
const char *vibe_card_input_route_name(vibe_card_input_route_t route);
bool vibe_card_input_route_parse(const char *name,
                                 vibe_card_input_route_t *route);
