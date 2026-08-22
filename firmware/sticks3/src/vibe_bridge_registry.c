#include "vibe_bridge_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#define BRIDGE_TARGET_NAMESPACE "vibe_bridge"
#define BRIDGE_TARGET_HOST_KEY "host"
#define BRIDGE_TARGET_PORT_KEY "port"
#define BRIDGE_TARGET_SSID_KEY "ssid"
#define BRIDGE_TARGET_PROFILE_KEY "profile"
#define BRIDGE_PROFILE_STORE_KEY "profiles"
#define BRIDGE_PROFILE_STORE_MAGIC 0x56424250u
#define BRIDGE_PROFILE_STORE_VERSION 1

static const char *TAG = "vibe_bridge_registry";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN];
    bridge_discovered_profile_t
        profiles[VIBE_BRIDGE_REGISTRY_MAX_PROFILES];
} bridge_profile_store_t;

static void profiles_lock(const vibe_bridge_registry_t *registry)
{
    if (registry->profiles_lock) {
        xSemaphoreTake(registry->profiles_lock, portMAX_DELAY);
    }
}

static void profiles_unlock(const vibe_bridge_registry_t *registry)
{
    if (registry->profiles_lock) {
        xSemaphoreGive(registry->profiles_lock);
    }
}

static void target_lock(const vibe_bridge_registry_t *registry)
{
    if (registry->target_lock) {
        xSemaphoreTake(registry->target_lock, portMAX_DELAY);
    }
}

static void target_unlock(const vibe_bridge_registry_t *registry)
{
    if (registry->target_lock) {
        xSemaphoreGive(registry->target_lock);
    }
}

static void current_ssid(const vibe_bridge_registry_t *registry,
                         char *ssid,
                         size_t ssid_len)
{
    if (!ssid || ssid_len == 0) {
        return;
    }
    ssid[0] = '\0';
    if (registry->config.current_ssid) {
        registry->config.current_ssid(
            ssid, ssid_len, registry->config.context);
    }
}

void vibe_bridge_registry_clear(vibe_bridge_registry_t *registry)
{
    if (!registry) {
        return;
    }
    profiles_lock(registry);
    memset(registry->profiles, 0, sizeof(registry->profiles));
    registry->profile_count = 0;
    profiles_unlock(registry);
}

static esp_err_t profiles_load(vibe_bridge_registry_t *registry,
                               const char *ssid)
{
    vibe_bridge_registry_clear(registry);
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err =
        nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    bridge_profile_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    size_t required_size = sizeof(*store);
    err = nvs_get_blob(
        handle, BRIDGE_PROFILE_STORE_KEY, store, &required_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(store);
        return err;
    }
    if (required_size != sizeof(*store) ||
        store->magic != BRIDGE_PROFILE_STORE_MAGIC ||
        store->version != BRIDGE_PROFILE_STORE_VERSION ||
        store->count == 0 ||
        store->count > VIBE_BRIDGE_REGISTRY_MAX_PROFILES ||
        strcmp(store->ssid, ssid) != 0) {
        free(store);
        return ESP_ERR_INVALID_VERSION;
    }

    const uint16_t restored_count = store->count;
    profiles_lock(registry);
    memcpy(registry->profiles, store->profiles,
           store->count * sizeof(store->profiles[0]));
    registry->profile_count = store->count;
    profiles_unlock(registry);
    free(store);
    ESP_LOGI(TAG, "bridge profiles restored ssid=%s count=%u",
             ssid, (unsigned)restored_count);
    return ESP_OK;
}

static esp_err_t profiles_save_for_ssid(
    const vibe_bridge_registry_t *registry,
    const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    bridge_profile_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        return ESP_ERR_NO_MEM;
    }
    store->magic = BRIDGE_PROFILE_STORE_MAGIC;
    store->version = BRIDGE_PROFILE_STORE_VERSION;
    profiles_lock(registry);
    if (registry->profile_count == 0) {
        profiles_unlock(registry);
        free(store);
        return ESP_ERR_INVALID_STATE;
    }
    store->count = (uint16_t)registry->profile_count;
    snprintf(store->ssid, sizeof(store->ssid), "%s", ssid);
    memcpy(store->profiles, registry->profiles,
           registry->profile_count * sizeof(store->profiles[0]));
    profiles_unlock(registry);

    nvs_handle_t handle;
    esp_err_t err =
        nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(store);
        return err;
    }
    err = nvs_set_blob(
        handle, BRIDGE_PROFILE_STORE_KEY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    const uint16_t saved_count = store->count;
    nvs_close(handle);
    free(store);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bridge profiles saved ssid=%s count=%u",
                 ssid, (unsigned)saved_count);
    }
    return err;
}

static esp_err_t profiles_save_current(
    const vibe_bridge_registry_t *registry,
    const char *expected_ssid)
{
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = "";
    current_ssid(registry, ssid, sizeof(ssid));
    if (ssid[0] == '\0' || !expected_ssid ||
        strcmp(ssid, expected_ssid) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return profiles_save_for_ssid(registry, ssid);
}

esp_err_t vibe_bridge_registry_init(
    vibe_bridge_registry_t *registry,
    const vibe_bridge_registry_config_t *config)
{
    if (!registry || !config || !config->configured_profiles ||
        config->configured_profile_count == 0 ||
        !config->default_profile_id || !config->default_host ||
        config->default_port <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(registry, 0, sizeof(*registry));
    registry->config = *config;
    registry->target_lock = xSemaphoreCreateMutex();
    registry->profiles_lock = xSemaphoreCreateMutex();
    registry->probe_lock = xSemaphoreCreateMutex();
    if (!registry->target_lock || !registry->profiles_lock ||
        !registry->probe_lock) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(registry->target.host, sizeof(registry->target.host), "%s",
             config->default_host);
    registry->target.port = config->default_port;
    snprintf(registry->target.profile_id,
             sizeof(registry->target.profile_id), "%s",
             config->default_profile_id);
    snprintf(registry->target.source, sizeof(registry->target.source),
             "boot");
    return ESP_OK;
}

void vibe_bridge_registry_target(const vibe_bridge_registry_t *registry,
                                 vibe_bridge_target_t *target)
{
    if (!registry || !target) {
        return;
    }
    target_lock(registry);
    *target = registry->target;
    target_unlock(registry);
}

size_t vibe_bridge_registry_profile_count(
    const vibe_bridge_registry_t *registry)
{
    if (!registry) {
        return 0;
    }
    profiles_lock(registry);
    const size_t count = registry->profile_count;
    profiles_unlock(registry);
    return count > 0 ? count : registry->config.configured_profile_count;
}

size_t vibe_bridge_registry_saved_profile_count(
    const vibe_bridge_registry_t *registry)
{
    if (!registry) {
        return 0;
    }
    profiles_lock(registry);
    const size_t count = registry->profile_count;
    profiles_unlock(registry);
    return count;
}

bool vibe_bridge_registry_saved_profile_snapshot(
    const vibe_bridge_registry_t *registry,
    size_t index,
    bridge_profile_snapshot_t *snapshot)
{
    if (!registry || !snapshot) {
        return false;
    }
    profiles_lock(registry);
    if (index >= registry->profile_count) {
        profiles_unlock(registry);
        return false;
    }
    vibe_bridge_profile_snapshot_from_discovered(
        &registry->profiles[index], snapshot);
    profiles_unlock(registry);
    return true;
}

bool vibe_bridge_registry_profile_snapshot(
    const vibe_bridge_registry_t *registry,
    size_t index,
    bridge_profile_snapshot_t *snapshot)
{
    if (!registry || !snapshot) {
        return false;
    }
    profiles_lock(registry);
    const size_t discovered_count = registry->profile_count;
    if (discovered_count > 0) {
        if (index >= discovered_count) {
            profiles_unlock(registry);
            return false;
        }
        vibe_bridge_profile_snapshot_from_discovered(
            &registry->profiles[index], snapshot);
        profiles_unlock(registry);
        return true;
    }
    profiles_unlock(registry);
    if (index >= registry->config.configured_profile_count) {
        return false;
    }
    vibe_bridge_profile_snapshot_from_config(
        &registry->config.configured_profiles[index], snapshot);
    return true;
}

bool vibe_bridge_registry_target_profile(
    const vibe_bridge_registry_t *registry,
    const vibe_bridge_target_t *target,
    bridge_profile_snapshot_t *snapshot)
{
    if (!registry || !target || !snapshot ||
        target->profile_id[0] == '\0') {
        return false;
    }
    profiles_lock(registry);
    for (size_t index = 0; index < registry->profile_count; ++index) {
        if (strcmp(registry->profiles[index].id,
                   target->profile_id) == 0) {
            vibe_bridge_profile_snapshot_from_discovered(
                &registry->profiles[index], snapshot);
            profiles_unlock(registry);
            return true;
        }
    }
    profiles_unlock(registry);
    for (size_t index = 0;
         index < registry->config.configured_profile_count; ++index) {
        const bridge_profile_config_t *profile =
            &registry->config.configured_profiles[index];
        if (profile->id &&
            strcmp(profile->id, target->profile_id) == 0) {
            vibe_bridge_profile_snapshot_from_config(profile, snapshot);
            return true;
        }
    }
    return false;
}

int vibe_bridge_registry_profile_index(
    const vibe_bridge_registry_t *registry,
    const char *profile_id)
{
    if (!registry || !profile_id || profile_id[0] == '\0') {
        return -1;
    }
    const size_t count = vibe_bridge_registry_profile_count(registry);
    for (size_t index = 0; index < count; ++index) {
        bridge_profile_snapshot_t profile;
        if (vibe_bridge_registry_profile_snapshot(
                registry, index, &profile) &&
            strcmp(profile.id, profile_id) == 0) {
            return (int)index;
        }
    }
    return -1;
}

bool vibe_bridge_registry_select(vibe_bridge_registry_t *registry,
                                 size_t profile_index,
                                 const char *source,
                                 bool available)
{
    bridge_profile_snapshot_t profile;
    if (!registry ||
        !vibe_bridge_registry_profile_snapshot(
            registry, profile_index, &profile) ||
        profile.id[0] == '\0' || profile.host[0] == '\0' ||
        profile.port <= 0) {
        return false;
    }
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = "";
    current_ssid(registry, ssid, sizeof(ssid));
    target_lock(registry);
    snprintf(registry->target.host, sizeof(registry->target.host), "%s",
             profile.host);
    registry->target.port = profile.port;
    registry->target.profile_index = profile_index;
    snprintf(registry->target.profile_id,
             sizeof(registry->target.profile_id), "%s", profile.id);
    snprintf(registry->target.source, sizeof(registry->target.source), "%s",
             source ? source : "runtime");
    snprintf(registry->target.ssid, sizeof(registry->target.ssid), "%s",
             ssid);
    registry->target.failure_count = 0;
    registry->target.available = available;
    target_unlock(registry);
    return true;
}

void vibe_bridge_registry_note_result(vibe_bridge_registry_t *registry,
                                      const char *expected_profile_id,
                                      esp_err_t result)
{
    if (!registry) {
        return;
    }
    target_lock(registry);
    if (expected_profile_id && expected_profile_id[0] != '\0' &&
        strcmp(registry->target.profile_id, expected_profile_id) != 0) {
        target_unlock(registry);
        ESP_LOGI(TAG, "bridge result ignored for stale profile id=%s",
                 expected_profile_id);
        return;
    }
    if (result == ESP_OK) {
        registry->target.failure_count = 0;
        registry->target.available = true;
    } else {
        registry->target.failure_count++;
        if (registry->target.failure_count >=
            registry->config.failure_threshold) {
            registry->target.available = false;
        }
    }
    target_unlock(registry);
}

static bool target_needs_selection(vibe_bridge_registry_t *registry)
{
    vibe_bridge_target_t target;
    bridge_profile_snapshot_t profile;
    vibe_bridge_registry_target(registry, &target);
    if (target.host[0] == '\0' || target.port <= 0 ||
        !vibe_bridge_registry_target_profile(
            registry, &target, &profile)) {
        return true;
    }
    return strcmp(target.host, profile.host) != 0 ||
           target.port != profile.port;
}

static esp_err_t target_load(vibe_bridge_registry_t *registry,
                             const char *current_network)
{
    nvs_handle_t handle;
    esp_err_t err =
        nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = "";
    char profile_id[VIBE_BRIDGE_PROFILE_ID_LEN] = "";
    size_t ssid_len = sizeof(ssid);
    size_t profile_id_len = sizeof(profile_id);
    err = nvs_get_str(handle, BRIDGE_TARGET_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, BRIDGE_TARGET_PROFILE_KEY,
                          profile_id, &profile_id_len);
    }
    nvs_close(handle);
    if (err != ESP_OK || strcmp(ssid, current_network) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const int index =
        vibe_bridge_registry_profile_index(registry, profile_id);
    if (index < 0 ||
        !vibe_bridge_registry_select(
            registry, (size_t)index, "nvs", false)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "bridge profile restored ssid=%s id=%s",
             current_network, profile_id);
    return ESP_OK;
}

esp_err_t vibe_bridge_registry_save_target(
    const vibe_bridge_registry_t *registry)
{
    if (!registry) {
        return ESP_ERR_INVALID_ARG;
    }
    vibe_bridge_target_t target;
    vibe_bridge_registry_target(registry, &target);
    if (target.profile_id[0] == '\0' || target.ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err =
        nvs_open(BRIDGE_TARGET_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open bridge target NVS for write");
    err = nvs_set_str(handle, BRIDGE_TARGET_SSID_KEY, target.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(
            handle, BRIDGE_TARGET_PROFILE_KEY, target.profile_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, BRIDGE_TARGET_HOST_KEY, target.host);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, BRIDGE_TARGET_PORT_KEY, target.port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bridge profile saved ssid=%s id=%s source=%s",
                 target.ssid, target.profile_id, target.source);
    }
    return err;
}

esp_err_t vibe_bridge_registry_ensure_target(
    vibe_bridge_registry_t *registry)
{
    if (!registry) {
        return ESP_ERR_INVALID_ARG;
    }
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = "";
    current_ssid(registry, ssid, sizeof(ssid));
    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(registry->loaded_ssid, ssid) != 0) {
        (void)profiles_load(registry, ssid);
        (void)target_load(registry, ssid);
        snprintf(registry->loaded_ssid, sizeof(registry->loaded_ssid),
                 "%s", ssid);
    }
    if (!target_needs_selection(registry)) {
        return ESP_OK;
    }
    if (!vibe_bridge_registry_select(registry, 0, "default", false)) {
        return ESP_ERR_NOT_FOUND;
    }
    return vibe_bridge_registry_save_target(registry);
}

bool vibe_bridge_registry_merge_scan(
    vibe_bridge_registry_t *registry,
    const char *scan_ssid,
    const bridge_discovered_profile_t *profiles,
    size_t profile_count)
{
    if (!registry || !profiles || profile_count == 0 ||
        !scan_ssid || scan_ssid[0] == '\0') {
        return false;
    }
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN] = "";
    current_ssid(registry, ssid, sizeof(ssid));
    if (strcmp(ssid, scan_ssid) != 0) {
        return false;
    }
    size_t skipped = 0;
    profiles_lock(registry);
    const bool changed = vibe_bridge_profiles_merge(
        registry->profiles, &registry->profile_count,
        VIBE_BRIDGE_REGISTRY_MAX_PROFILES,
        profiles, profile_count, &skipped);
    profiles_unlock(registry);
    if (skipped > 0) {
        ESP_LOGW(TAG, "bridge profile store full; skipped=%u",
                 (unsigned)skipped);
    }
    if (changed) {
        esp_err_t err = profiles_save_current(registry, scan_ssid);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bridge profile NVS save skipped/failed: %s",
                     esp_err_to_name(err));
        }
    }
    return changed;
}

esp_err_t vibe_bridge_registry_upsert_manual(
    vibe_bridge_registry_t *registry,
    const char *ssid,
    const bridge_discovered_profile_t *profile,
    const char *source)
{
    if (!registry || !ssid || ssid[0] == '\0' || !profile ||
        profile->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t profile_index = 0;
    profiles_lock(registry);
    if (strcmp(registry->loaded_ssid, ssid) != 0) {
        memset(registry->profiles, 0, sizeof(registry->profiles));
        registry->profile_count = 0;
    }
    bool found = false;
    for (size_t index = 0; index < registry->profile_count; ++index) {
        if (strcmp(registry->profiles[index].id, profile->id) == 0) {
            profile_index = index;
            found = true;
            break;
        }
    }
    if (!found) {
        if (registry->profile_count >=
            VIBE_BRIDGE_REGISTRY_MAX_PROFILES) {
            profile_index = registry->profile_count - 1;
        } else {
            profile_index = registry->profile_count++;
        }
    }
    registry->profiles[profile_index] = *profile;
    snprintf(registry->loaded_ssid, sizeof(registry->loaded_ssid),
             "%s", ssid);
    profiles_unlock(registry);

    ESP_RETURN_ON_ERROR(
        profiles_save_for_ssid(registry, ssid),
        TAG, "save manual bridge");
    if (!vibe_bridge_registry_select(
            registry, profile_index, source, false)) {
        return ESP_ERR_INVALID_STATE;
    }
    return vibe_bridge_registry_save_target(registry);
}

void vibe_bridge_registry_probe_lock(vibe_bridge_registry_t *registry)
{
    if (registry && registry->probe_lock) {
        xSemaphoreTake(registry->probe_lock, portMAX_DELAY);
    }
}

void vibe_bridge_registry_probe_unlock(vibe_bridge_registry_t *registry)
{
    if (registry && registry->probe_lock) {
        xSemaphoreGive(registry->probe_lock);
    }
}
