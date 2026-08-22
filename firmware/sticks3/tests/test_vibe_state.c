#include "vibe_app_state.h"
#include "vibe_state_json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults_and_provider_selection(void)
{
    vibe_app_state_t state;
    vibe_app_state_init(&state);
    assert(strcmp(state.time, "--:--") == 0);
    assert(state.current_provider == VIBE_PROVIDER_CODEX);
    assert(strcmp(vibe_app_state_current_provider_const(&state)->status,
                  "OFFLINE") == 0);

    assert(vibe_app_state_select_provider_key(&state, "claude", true));
    assert(state.current_provider == VIBE_PROVIDER_CLAUDE);
    assert(state.provider_manually_selected);
    assert(vibe_app_state_next_provider(state.current_provider) ==
           VIBE_PROVIDER_CODEX);
    assert(!vibe_app_state_select_provider_key(&state, "unknown", false));
}

static void test_unwrapped_state_and_percent_normalization(void)
{
    vibe_app_state_t state;
    vibe_app_state_init(&state);
    vibe_state_update_t update;
    assert(vibe_state_json_apply(
        &state,
        "{\"time\":\"13:01\",\"wifi\":true,\"ble\":false,"
        "\"active_provider\":\"claude\",\"provider\":{\"id\":\"claude\","
        "\"status\":\"RUNNING\",\"project\":\"VibeStick\","
        "\"quota_5h_remaining\":\"105%\",\"quota_7d_remaining\":\"-3\","
        "\"quota_updated_at\":\"13:00\",\"quota_stale\":true},"
        "\"alert\":{\"event_id\":\"a1\",\"type\":\"DONE\","
        "\"message\":\"ready\"},\"tts_playback_request_id\":\"tts-1\"}",
        &update));
    assert(state.wifi);
    assert(!state.ble);
    assert(state.current_provider == VIBE_PROVIDER_CLAUDE);
    const vibe_provider_state_t *provider =
        vibe_app_state_current_provider_const(&state);
    assert(strcmp(provider->status, "RUNNING") == 0);
    assert(provider->quota_5h_valid && provider->quota_5h == 100);
    assert(provider->quota_7d_valid && provider->quota_7d == 0);
    assert(provider->quota_stale);
    assert(update.provider_changed);
    assert(update.alert_changed);
    assert(update.tts_requested);

    assert(vibe_state_json_apply(
        &state, "{\"tts_playback_request_id\":\"tts-1\"}", &update));
    assert(!update.tts_requested);
}

static void test_wrapped_legacy_codex_and_manual_provider(void)
{
    vibe_app_state_t state;
    vibe_app_state_init(&state);
    assert(vibe_app_state_select_provider_key(&state, "claude", true));
    vibe_state_update_t update;
    assert(vibe_state_json_apply(
        &state,
        "{\"state\":{\"active_provider\":\"codex\","
        "\"provider\":{\"id\":\"codex\",\"status\":\"DONE\","
        "\"quota_5h_remaining\":null,\"quota_7d_remaining\":\"bad\"},"
        "\"codex\":{\"status\":\"RUNNING\",\"quota_5h_remaining\":42,"
        "\"quota_7d_remaining\":\"81%\"}}}",
        &update));
    assert(state.current_provider == VIBE_PROVIDER_CLAUDE);
    const vibe_provider_state_t *codex =
        vibe_app_state_provider_const(&state, VIBE_PROVIDER_CODEX);
    assert(strcmp(codex->status, "RUNNING") == 0);
    assert(codex->quota_5h_valid && codex->quota_5h == 42);
    assert(codex->quota_7d_valid && codex->quota_7d == 81);
    assert(!update.provider_changed);
}

static void test_missing_fields_and_invalid_json(void)
{
    vibe_app_state_t state;
    vibe_app_state_init(&state);
    strcpy(state.alert_type, "APPROVAL");
    vibe_state_update_t update;
    assert(vibe_state_json_apply(&state, "{\"wifi\":\"yes\"}", &update));
    assert(!state.wifi);
    assert(strcmp(state.alert_type, "APPROVAL") == 0);
    assert(!update.alert_changed);
    assert(!vibe_state_json_apply(&state, "{", &update));
}

int main(void)
{
    test_defaults_and_provider_selection();
    test_unwrapped_state_and_percent_normalization();
    test_wrapped_legacy_codex_and_manual_provider();
    test_missing_fields_and_invalid_json();
    puts("vibestick state tests passed");
    return 0;
}

