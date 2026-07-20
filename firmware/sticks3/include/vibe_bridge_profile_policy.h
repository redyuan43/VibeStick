#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIBE_BRIDGE_PROFILE_ID_LEN 65
#define VIBE_BRIDGE_PROFILE_LABEL_LEN 65
#define VIBE_BRIDGE_PROFILE_HOST_LEN 64
#define VIBE_BRIDGE_PROFILE_TOKEN_LEN 65

typedef struct {
    const char *id;
    const char *label;
    const char *host;
    int port;
    const char *token;
} bridge_profile_config_t;

typedef struct {
    char id[VIBE_BRIDGE_PROFILE_ID_LEN];
    char label[VIBE_BRIDGE_PROFILE_LABEL_LEN];
    char host[VIBE_BRIDGE_PROFILE_HOST_LEN];
    int32_t port;
    char token[VIBE_BRIDGE_PROFILE_TOKEN_LEN];
} bridge_discovered_profile_t;

typedef struct {
    char id[VIBE_BRIDGE_PROFILE_ID_LEN];
    char label[VIBE_BRIDGE_PROFILE_LABEL_LEN];
    char host[VIBE_BRIDGE_PROFILE_HOST_LEN];
    int port;
    char token[VIBE_BRIDGE_PROFILE_TOKEN_LEN];
} bridge_profile_snapshot_t;

bool vibe_bridge_health_name_supported(const char *bridge_name);
bool vibe_bridge_identity_is_generic(const char *bridge_id);
void vibe_bridge_fallback_id(const char *host, char *id, size_t id_len);
void vibe_bridge_profile_snapshot_from_config(
    const bridge_profile_config_t *profile,
    bridge_profile_snapshot_t *snapshot);
void vibe_bridge_profile_snapshot_from_discovered(
    const bridge_discovered_profile_t *profile,
    bridge_profile_snapshot_t *snapshot);
void vibe_bridge_profile_snapshot_view(
    const bridge_profile_snapshot_t *snapshot,
    bridge_profile_config_t *view);
bool vibe_bridge_discovered_profile_equal(
    const bridge_discovered_profile_t *left,
    const bridge_discovered_profile_t *right);
int vibe_bridge_discovered_profile_find(
    const bridge_discovered_profile_t *profiles,
    size_t count,
    const bridge_discovered_profile_t *profile);
bool vibe_bridge_profiles_merge(
    bridge_discovered_profile_t *stored,
    size_t *stored_count,
    size_t capacity,
    const bridge_discovered_profile_t *scanned,
    size_t scanned_count,
    size_t *skipped_full);
