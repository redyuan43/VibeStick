#include "vibe_app_state.h"

#include <string.h>

static const char *const s_provider_keys[VIBE_PROVIDER_COUNT] = {
    [VIBE_PROVIDER_CODEX] = "codex",
    [VIBE_PROVIDER_CLAUDE] = "claude",
};

void vibe_app_state_init(vibe_app_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    strcpy(state->time, "--:--");
    strcpy(state->providers[VIBE_PROVIDER_CODEX].status, "OFFLINE");
    strcpy(state->providers[VIBE_PROVIDER_CODEX].project, "vibestick");
    strcpy(state->providers[VIBE_PROVIDER_CLAUDE].status, "OFFLINE");
    strcpy(state->providers[VIBE_PROVIDER_CLAUDE].project, "vibestick");
    state->current_provider = VIBE_PROVIDER_CODEX;
    strcpy(state->alert_type, "NONE");
}

const char *vibe_provider_key(vibe_provider_id_t provider)
{
    if ((int)provider < 0 || provider >= VIBE_PROVIDER_COUNT) {
        return s_provider_keys[VIBE_PROVIDER_CODEX];
    }
    return s_provider_keys[provider];
}

bool vibe_provider_from_key(const char *key, vibe_provider_id_t *provider)
{
    if (!key || key[0] == '\0') {
        return false;
    }
    for (int candidate = 0; candidate < VIBE_PROVIDER_COUNT; ++candidate) {
        if (strcmp(key, s_provider_keys[candidate]) == 0) {
            if (provider) {
                *provider = (vibe_provider_id_t)candidate;
            }
            return true;
        }
    }
    return false;
}

vibe_provider_id_t vibe_app_state_next_provider(vibe_provider_id_t current)
{
    if ((int)current < 0 || current >= VIBE_PROVIDER_COUNT) {
        return VIBE_PROVIDER_CODEX;
    }
    return (vibe_provider_id_t)((current + 1) % VIBE_PROVIDER_COUNT);
}

bool vibe_app_state_select_provider(vibe_app_state_t *state,
                                    vibe_provider_id_t provider,
                                    bool manual)
{
    if (!state || (int)provider < 0 || provider >= VIBE_PROVIDER_COUNT) {
        return false;
    }
    state->current_provider = provider;
    if (manual) {
        state->provider_manually_selected = true;
    }
    return true;
}

bool vibe_app_state_select_provider_key(vibe_app_state_t *state,
                                        const char *key,
                                        bool manual)
{
    vibe_provider_id_t provider = VIBE_PROVIDER_CODEX;
    return vibe_provider_from_key(key, &provider) &&
           vibe_app_state_select_provider(state, provider, manual);
}

vibe_provider_state_t *vibe_app_state_provider(vibe_app_state_t *state,
                                               vibe_provider_id_t provider)
{
    if (!state) {
        return NULL;
    }
    if ((int)provider < 0 || provider >= VIBE_PROVIDER_COUNT) {
        provider = VIBE_PROVIDER_CODEX;
    }
    return &state->providers[provider];
}

const vibe_provider_state_t *vibe_app_state_provider_const(
    const vibe_app_state_t *state,
    vibe_provider_id_t provider)
{
    return vibe_app_state_provider((vibe_app_state_t *)state, provider);
}

vibe_provider_state_t *vibe_app_state_current_provider(vibe_app_state_t *state)
{
    return state ? vibe_app_state_provider(state, state->current_provider) : NULL;
}

const vibe_provider_state_t *vibe_app_state_current_provider_const(
    const vibe_app_state_t *state)
{
    return state
               ? vibe_app_state_provider_const(state, state->current_provider)
               : NULL;
}

