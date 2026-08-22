#pragma once

#include <stdbool.h>

#include "vibe_app_state.h"

typedef struct {
    bool provider_changed;
    bool alert_changed;
    bool tts_requested;
} vibe_state_update_t;

bool vibe_state_json_apply(vibe_app_state_t *state,
                           const char *json,
                           vibe_state_update_t *update);

