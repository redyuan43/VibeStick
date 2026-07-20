#include "vibe_bridge_profile_policy.h"

#include <string.h>

static void copy_string(char *target, size_t target_len, const char *source)
{
    if (!target || target_len == 0) {
        return;
    }
    size_t index = 0;
    const char *value = source ? source : "";
    while (value[index] != '\0' && index + 1 < target_len) {
        target[index] = value[index];
        index++;
    }
    target[index] = '\0';
}

bool vibe_bridge_health_name_supported(const char *bridge_name)
{
    return bridge_name &&
           (strcmp(bridge_name, "vibestick-bridge") == 0 ||
            strcmp(bridge_name, "capswriter-m5-voice-bridge") == 0);
}

bool vibe_bridge_identity_is_generic(const char *bridge_id)
{
    return !bridge_id || bridge_id[0] == '\0' ||
           strcmp(bridge_id, "vibestick-bridge") == 0 ||
           strcmp(bridge_id, "capswriter-m5-voice-bridge") == 0;
}

void vibe_bridge_fallback_id(const char *host, char *id, size_t id_len)
{
    if (!id || id_len == 0) {
        return;
    }
    const char *source = host && host[0] != '\0' ? host : "bridge";
    size_t used = 0;
    const char prefix[] = "lan-";
    for (size_t index = 0; index < sizeof(prefix) - 1 && used + 1 < id_len; index++) {
        id[used++] = prefix[index];
    }
    for (size_t index = 0; source[index] != '\0' && used + 1 < id_len; index++) {
        id[used++] = source[index] == '.' ? '-' : source[index];
    }
    id[used] = '\0';
}

void vibe_bridge_profile_snapshot_from_config(
    const bridge_profile_config_t *profile,
    bridge_profile_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!profile) {
        return;
    }
    copy_string(snapshot->id, sizeof(snapshot->id), profile->id);
    copy_string(snapshot->label, sizeof(snapshot->label), profile->label);
    copy_string(snapshot->host, sizeof(snapshot->host), profile->host);
    snapshot->port = profile->port;
    copy_string(snapshot->token, sizeof(snapshot->token), profile->token);
}

void vibe_bridge_profile_snapshot_from_discovered(
    const bridge_discovered_profile_t *profile,
    bridge_profile_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!profile) {
        return;
    }
    copy_string(snapshot->id, sizeof(snapshot->id), profile->id);
    copy_string(snapshot->label, sizeof(snapshot->label), profile->label);
    copy_string(snapshot->host, sizeof(snapshot->host), profile->host);
    snapshot->port = (int)profile->port;
    copy_string(snapshot->token, sizeof(snapshot->token), profile->token);
}

void vibe_bridge_profile_snapshot_view(
    const bridge_profile_snapshot_t *snapshot,
    bridge_profile_config_t *view)
{
    if (!view) {
        return;
    }
    if (!snapshot) {
        *view = (bridge_profile_config_t){0};
        return;
    }
    *view = (bridge_profile_config_t){
        .id = snapshot->id,
        .label = snapshot->label,
        .host = snapshot->host,
        .port = snapshot->port,
        .token = snapshot->token,
    };
}

bool vibe_bridge_discovered_profile_equal(
    const bridge_discovered_profile_t *left,
    const bridge_discovered_profile_t *right)
{
    return left && right &&
           strcmp(left->id, right->id) == 0 &&
           strcmp(left->label, right->label) == 0 &&
           strcmp(left->host, right->host) == 0 &&
           left->port == right->port &&
           strcmp(left->token, right->token) == 0;
}

int vibe_bridge_discovered_profile_find(
    const bridge_discovered_profile_t *profiles,
    size_t count,
    const bridge_discovered_profile_t *profile)
{
    if (!profiles || !profile || profile->id[0] == '\0') {
        return -1;
    }
    for (size_t index = 0; index < count; index++) {
        const bridge_discovered_profile_t *stored = &profiles[index];
        if ((stored->id[0] != '\0' && strcmp(stored->id, profile->id) == 0) ||
            (strcmp(stored->host, profile->host) == 0 &&
             stored->port == profile->port)) {
            return (int)index;
        }
    }
    return -1;
}

bool vibe_bridge_profiles_merge(
    bridge_discovered_profile_t *stored,
    size_t *stored_count,
    size_t capacity,
    const bridge_discovered_profile_t *scanned,
    size_t scanned_count,
    size_t *skipped_full)
{
    if (skipped_full) {
        *skipped_full = 0;
    }
    if (!stored || !stored_count || !scanned || *stored_count > capacity) {
        return false;
    }

    bool changed = false;
    for (size_t scan_index = 0; scan_index < scanned_count; scan_index++) {
        const bridge_discovered_profile_t *profile = &scanned[scan_index];
        int existing =
            vibe_bridge_discovered_profile_find(stored, *stored_count, profile);
        if (existing >= 0) {
            bridge_discovered_profile_t *current = &stored[(size_t)existing];
            if (!vibe_bridge_discovered_profile_equal(current, profile)) {
                *current = *profile;
                changed = true;
            }
            continue;
        }
        if (*stored_count >= capacity) {
            if (skipped_full) {
                (*skipped_full)++;
            }
            continue;
        }
        stored[(*stored_count)++] = *profile;
        changed = true;
    }
    return changed;
}
