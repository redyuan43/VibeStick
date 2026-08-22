#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vibe_bridge_profile_policy.h"
#include "vibe_wifi_policy.h"

#define VIBE_BRIDGE_TARGET_SOURCE_LEN 12
#define VIBE_BRIDGE_REGISTRY_MAX_PROFILES 8

typedef struct {
    char host[VIBE_BRIDGE_PROFILE_HOST_LEN];
    int port;
    size_t profile_index;
    char profile_id[VIBE_BRIDGE_PROFILE_ID_LEN];
    char source[VIBE_BRIDGE_TARGET_SOURCE_LEN];
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN];
    int failure_count;
    bool available;
} vibe_bridge_target_t;

typedef void (*vibe_bridge_registry_ssid_fn)(char *ssid,
                                             size_t ssid_len,
                                             void *context);

typedef struct {
    const bridge_profile_config_t *configured_profiles;
    size_t configured_profile_count;
    const char *default_profile_id;
    const char *default_host;
    int default_port;
    int failure_threshold;
    vibe_bridge_registry_ssid_fn current_ssid;
    void *context;
} vibe_bridge_registry_config_t;

typedef struct {
    vibe_bridge_registry_config_t config;
    SemaphoreHandle_t target_lock;
    SemaphoreHandle_t profiles_lock;
    SemaphoreHandle_t probe_lock;
    bridge_discovered_profile_t
        profiles[VIBE_BRIDGE_REGISTRY_MAX_PROFILES];
    size_t profile_count;
    char loaded_ssid[VIBE_WIFI_PROFILE_SSID_LEN];
    vibe_bridge_target_t target;
} vibe_bridge_registry_t;

esp_err_t vibe_bridge_registry_init(
    vibe_bridge_registry_t *registry,
    const vibe_bridge_registry_config_t *config);
void vibe_bridge_registry_target(const vibe_bridge_registry_t *registry,
                                 vibe_bridge_target_t *target);
size_t vibe_bridge_registry_profile_count(
    const vibe_bridge_registry_t *registry);
size_t vibe_bridge_registry_saved_profile_count(
    const vibe_bridge_registry_t *registry);
bool vibe_bridge_registry_profile_snapshot(
    const vibe_bridge_registry_t *registry,
    size_t index,
    bridge_profile_snapshot_t *snapshot);
bool vibe_bridge_registry_saved_profile_snapshot(
    const vibe_bridge_registry_t *registry,
    size_t index,
    bridge_profile_snapshot_t *snapshot);
bool vibe_bridge_registry_target_profile(
    const vibe_bridge_registry_t *registry,
    const vibe_bridge_target_t *target,
    bridge_profile_snapshot_t *snapshot);
int vibe_bridge_registry_profile_index(
    const vibe_bridge_registry_t *registry,
    const char *profile_id);
bool vibe_bridge_registry_select(vibe_bridge_registry_t *registry,
                                 size_t profile_index,
                                 const char *source,
                                 bool available);
void vibe_bridge_registry_note_result(vibe_bridge_registry_t *registry,
                                      const char *expected_profile_id,
                                      esp_err_t result);
esp_err_t vibe_bridge_registry_ensure_target(
    vibe_bridge_registry_t *registry);
esp_err_t vibe_bridge_registry_save_target(
    const vibe_bridge_registry_t *registry);
bool vibe_bridge_registry_merge_scan(
    vibe_bridge_registry_t *registry,
    const char *scan_ssid,
    const bridge_discovered_profile_t *profiles,
    size_t profile_count);
esp_err_t vibe_bridge_registry_upsert_manual(
    vibe_bridge_registry_t *registry,
    const char *ssid,
    const bridge_discovered_profile_t *profile,
    const char *source);
void vibe_bridge_registry_clear(vibe_bridge_registry_t *registry);
void vibe_bridge_registry_probe_lock(vibe_bridge_registry_t *registry);
void vibe_bridge_registry_probe_unlock(vibe_bridge_registry_t *registry);
