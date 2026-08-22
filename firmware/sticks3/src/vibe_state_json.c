#include "vibe_state_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void copy_json_string(cJSON *object,
                             const char *key,
                             char *target,
                             size_t target_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(target, target_len, "%s", item->valuestring);
    }
}

static bool json_percent_value(cJSON *item, int *value)
{
    if (cJSON_IsNumber(item)) {
        *value = item->valueint;
    } else if (cJSON_IsString(item) && item->valuestring &&
               item->valuestring[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(item->valuestring, &end, 10);
        if (!end || end == item->valuestring) {
            return false;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' ||
               *end == '\n' || *end == '%') {
            end++;
        }
        if (*end != '\0') {
            return false;
        }
        *value = (int)parsed;
    } else {
        return false;
    }
    if (*value < 0) {
        *value = 0;
    } else if (*value > 100) {
        *value = 100;
    }
    return true;
}

static void parse_provider_fields(cJSON *source, vibe_provider_state_t *target)
{
    copy_json_string(source, "status", target->status, sizeof(target->status));
    copy_json_string(source, "project", target->project, sizeof(target->project));
    copy_json_string(source, "quota_updated_at", target->quota_updated_at,
                     sizeof(target->quota_updated_at));

    cJSON *quota_5h =
        cJSON_GetObjectItemCaseSensitive(source, "quota_5h_remaining");
    cJSON *quota_7d =
        cJSON_GetObjectItemCaseSensitive(source, "quota_7d_remaining");
    cJSON *stale = cJSON_GetObjectItemCaseSensitive(source, "quota_stale");
    int quota_value = 0;
    target->quota_5h_valid = json_percent_value(quota_5h, &quota_value);
    if (target->quota_5h_valid) {
        target->quota_5h = quota_value;
    }
    target->quota_7d_valid = json_percent_value(quota_7d, &quota_value);
    if (target->quota_7d_valid) {
        target->quota_7d = quota_value;
    }
    target->quota_stale = cJSON_IsBool(stale) ? cJSON_IsTrue(stale) : false;
}

static void parse_provider_json(vibe_app_state_t *state,
                                cJSON *state_root,
                                cJSON *provider)
{
    char provider_key[16] = "";
    copy_json_string(provider, "id", provider_key, sizeof(provider_key));
    if (provider_key[0] == '\0') {
        copy_json_string(state_root, "active_provider", provider_key,
                         sizeof(provider_key));
    }

    vibe_provider_id_t provider_id = state->current_provider;
    if (provider_key[0] != '\0' &&
        vibe_provider_from_key(provider_key, &provider_id) &&
        !state->provider_manually_selected) {
        state->current_provider = provider_id;
    }
    parse_provider_fields(
        provider, vibe_app_state_provider(state, provider_id));
}

bool vibe_state_json_apply(vibe_app_state_t *state,
                           const char *json,
                           vibe_state_update_t *update)
{
    if (!state || !json) {
        return false;
    }
    vibe_state_update_t local_update = {0};
    if (!update) {
        update = &local_update;
    }
    memset(update, 0, sizeof(*update));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    cJSON *state_root = root;
    cJSON *wrapped_state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsObject(wrapped_state)) {
        state_root = wrapped_state;
    }

    const vibe_provider_id_t previous_provider = state->current_provider;
    char previous_alert_event_id[sizeof(state->alert_event_id)];
    char previous_alert_type[sizeof(state->alert_type)];
    char previous_alert_message[sizeof(state->alert_message)];
    memcpy(previous_alert_event_id, state->alert_event_id,
           sizeof(previous_alert_event_id));
    memcpy(previous_alert_type, state->alert_type, sizeof(previous_alert_type));
    memcpy(previous_alert_message, state->alert_message,
           sizeof(previous_alert_message));

    copy_json_string(state_root, "time", state->time, sizeof(state->time));
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(state_root, "wifi");
    cJSON *ble = cJSON_GetObjectItemCaseSensitive(state_root, "ble");
    state->wifi = cJSON_IsBool(wifi) ? cJSON_IsTrue(wifi) : state->wifi;
    state->ble = cJSON_IsBool(ble) ? cJSON_IsTrue(ble) : state->ble;

    cJSON *provider = cJSON_GetObjectItemCaseSensitive(state_root, "provider");
    cJSON *codex = cJSON_GetObjectItemCaseSensitive(state_root, "codex");
    if (cJSON_IsObject(provider)) {
        parse_provider_json(state, state_root, provider);
    } else {
        char active_provider[16] = "";
        copy_json_string(state_root, "active_provider", active_provider,
                         sizeof(active_provider));
        if (active_provider[0] != '\0' &&
            !state->provider_manually_selected) {
            (void)vibe_app_state_select_provider_key(
                state, active_provider, false);
        }
    }
    if (cJSON_IsObject(codex)) {
        parse_provider_fields(
            codex, vibe_app_state_provider(state, VIBE_PROVIDER_CODEX));
    }

    cJSON *alert = cJSON_GetObjectItemCaseSensitive(state_root, "alert");
    if (cJSON_IsObject(alert)) {
        copy_json_string(alert, "event_id", state->alert_event_id,
                         sizeof(state->alert_event_id));
        copy_json_string(alert, "type", state->alert_type,
                         sizeof(state->alert_type));
        copy_json_string(alert, "message", state->alert_message,
                         sizeof(state->alert_message));
    }

    char tts_request_id[sizeof(state->tts_playback_request_id)] = "";
    copy_json_string(state_root, "tts_playback_request_id", tts_request_id,
                     sizeof(tts_request_id));
    if (tts_request_id[0] != '\0' &&
        strcmp(tts_request_id, state->tts_playback_request_id) != 0) {
        snprintf(state->tts_playback_request_id,
                 sizeof(state->tts_playback_request_id), "%s",
                 tts_request_id);
        update->tts_requested = true;
    }

    update->provider_changed = previous_provider != state->current_provider;
    update->alert_changed =
        strcmp(previous_alert_event_id, state->alert_event_id) != 0 ||
        strcmp(previous_alert_type, state->alert_type) != 0 ||
        strcmp(previous_alert_message, state->alert_message) != 0;
    cJSON_Delete(root);
    return true;
}
