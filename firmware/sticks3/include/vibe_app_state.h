#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VIBE_PROVIDER_CODEX = 0,
    VIBE_PROVIDER_CLAUDE,
    VIBE_PROVIDER_COUNT,
} vibe_provider_id_t;

typedef struct {
    char status[24];
    char project[40];
    int quota_5h;
    int quota_7d;
    bool quota_5h_valid;
    bool quota_7d_valid;
    char quota_updated_at[8];
    bool quota_stale;
} vibe_provider_state_t;

typedef struct {
    char time[8];
    bool wifi;
    bool ble;
    int battery;
    bool battery_charging;
    bool usb_powered;
    char wifi_ip[16];
    vibe_provider_state_t providers[VIBE_PROVIDER_COUNT];
    vibe_provider_id_t current_provider;
    bool provider_manually_selected;
    char alert_event_id[56];
    char alert_type[24];
    char alert_message[80];
    char tts_playback_request_id[56];
} vibe_app_state_t;

void vibe_app_state_init(vibe_app_state_t *state);
const char *vibe_provider_key(vibe_provider_id_t provider);
bool vibe_provider_from_key(const char *key, vibe_provider_id_t *provider);
vibe_provider_id_t vibe_app_state_next_provider(vibe_provider_id_t current);
bool vibe_app_state_select_provider(vibe_app_state_t *state,
                                    vibe_provider_id_t provider,
                                    bool manual);
bool vibe_app_state_select_provider_key(vibe_app_state_t *state,
                                        const char *key,
                                        bool manual);
vibe_provider_state_t *vibe_app_state_provider(vibe_app_state_t *state,
                                               vibe_provider_id_t provider);
const vibe_provider_state_t *vibe_app_state_provider_const(
    const vibe_app_state_t *state,
    vibe_provider_id_t provider);
vibe_provider_state_t *vibe_app_state_current_provider(vibe_app_state_t *state);
const vibe_provider_state_t *vibe_app_state_current_provider_const(
    const vibe_app_state_t *state);

