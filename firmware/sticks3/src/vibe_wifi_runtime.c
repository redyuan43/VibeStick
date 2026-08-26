#include "vibe_wifi_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#define WIFI_PROFILE_NAMESPACE "vibe_wifi"
#define WIFI_PROFILE_BLOB_KEY "profiles"
#define WIFI_PROFILE_MAGIC 0x56425746u
#define WIFI_PROFILE_STORE_VERSION 1
#define WIFI_PROFILE_RETRY_LIMIT 2
#define WIFI_RECONNECT_MAX_MS 30000

static const char *TAG = "vibe_wifi";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    vibe_wifi_profile_t profiles[VIBE_WIFI_RUNTIME_MAX_PROFILES];
} wifi_profile_store_t;

static bool profile_merge(vibe_wifi_runtime_t *runtime,
                          const vibe_wifi_profile_t *profile)
{
    vibe_wifi_profile_merge_result_t result = vibe_wifi_profiles_merge(
        runtime->profiles, &runtime->profile_count,
        VIBE_WIFI_RUNTIME_MAX_PROFILES, profile);
    switch (result) {
    case VIBE_WIFI_PROFILE_UPDATED:
        ESP_LOGI(TAG, "updated stored Wi-Fi profile ssid=%s", profile->ssid);
        return true;
    case VIBE_WIFI_PROFILE_ADDED:
        ESP_LOGI(TAG, "added stored Wi-Fi profile ssid=%s", profile->ssid);
        return true;
    case VIBE_WIFI_PROFILE_FULL:
        ESP_LOGW(TAG, "Wi-Fi profile store full; ignoring ssid=%s",
                 profile->ssid);
        return false;
    case VIBE_WIFI_PROFILE_INVALID:
    case VIBE_WIFI_PROFILE_UNCHANGED:
    default:
        return false;
    }
}

static esp_err_t profiles_load(vibe_wifi_runtime_t *runtime)
{
    runtime->profile_count = 0;
    runtime->profile_index = 0;
    runtime->profile_retry_count = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_PROFILE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open Wi-Fi profile NVS");

    wifi_profile_store_t store = {0};
    size_t required_size = sizeof(store);
    err = nvs_get_blob(handle, WIFI_PROFILE_BLOB_KEY, &store, &required_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read Wi-Fi profile NVS");
    if (required_size != sizeof(store) ||
        store.magic != WIFI_PROFILE_MAGIC ||
        store.version != WIFI_PROFILE_STORE_VERSION ||
        store.count > VIBE_WIFI_RUNTIME_MAX_PROFILES) {
        ESP_LOGW(TAG, "ignoring invalid Wi-Fi profile store");
        return ESP_OK;
    }
    for (size_t index = 0; index < store.count; ++index) {
        (void)profile_merge(runtime, &store.profiles[index]);
    }
    ESP_LOGI(TAG, "loaded %u Wi-Fi profile(s) from NVS",
             (unsigned)runtime->profile_count);
    return ESP_OK;
}

static esp_err_t profiles_save(const vibe_wifi_runtime_t *runtime)
{
    wifi_profile_store_t store = {
        .magic = WIFI_PROFILE_MAGIC,
        .version = WIFI_PROFILE_STORE_VERSION,
        .count = (uint16_t)runtime->profile_count,
    };
    for (size_t index = 0; index < runtime->profile_count; ++index) {
        vibe_wifi_profile_copy(&store.profiles[index],
                               &runtime->profiles[index]);
    }

    nvs_handle_t handle;
    esp_err_t err =
        nvs_open(WIFI_PROFILE_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open Wi-Fi profile NVS for write");
    err = nvs_set_blob(handle, WIFI_PROFILE_BLOB_KEY, &store, sizeof(store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "write Wi-Fi profile NVS");
    ESP_LOGI(TAG, "saved %u Wi-Fi profile(s) to NVS",
             (unsigned)runtime->profile_count);
    return ESP_OK;
}

static esp_err_t apply_profile(vibe_wifi_runtime_t *runtime, size_t index)
{
    if (index >= runtime->profile_count) {
        return ESP_ERR_INVALID_STATE;
    }
    const vibe_wifi_profile_t *profile = &runtime->profiles[index];
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s",
             profile->ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s",
             profile->password);
    config.sta.threshold.authmode =
        profile->password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "using Wi-Fi profile %u/%u ssid=%s",
             (unsigned)(index + 1), (unsigned)runtime->profile_count,
             profile->ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static esp_err_t configure_started_radio(vibe_wifi_runtime_t *runtime)
{
    if (runtime->config.max_tx_power > 0) {
        ESP_RETURN_ON_ERROR(
            esp_wifi_set_max_tx_power(runtime->config.max_tx_power),
            TAG, "wifi tx power");
        ESP_LOGI(TAG, "Wi-Fi max TX power set to %.2f dBm",
                 runtime->config.max_tx_power / 4.0);
    }
    return esp_wifi_set_ps(runtime->config.idle_power_save);
}

static void notify_status(vibe_wifi_runtime_t *runtime)
{
    if (runtime->config.status_changed) {
        runtime->config.status_changed(
            atomic_load(&runtime->connected), runtime->ip,
            runtime->config.context);
    }
}

static void reconnect_timer_cb(void *arg)
{
    vibe_wifi_runtime_t *runtime = (vibe_wifi_runtime_t *)arg;
    if (!vibe_wifi_runtime_connected(runtime)) {
        ESP_LOGI(TAG, "Wi-Fi reconnect attempt=%u",
                 runtime->reconnect_attempt);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }
}

static void schedule_reconnect(vibe_wifi_runtime_t *runtime)
{
    if (!runtime->reconnect_timer) {
        return;
    }
    uint32_t delay_ms = vibe_wifi_reconnect_delay_ms(
        runtime->reconnect_attempt, WIFI_RECONNECT_MAX_MS);
    if (runtime->reconnect_attempt < UINT32_MAX) {
        runtime->reconnect_attempt++;
    }
    if (esp_timer_is_active(runtime->reconnect_timer)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_stop(runtime->reconnect_timer));
    }
    ESP_LOGI(TAG, "Wi-Fi reconnect scheduled in %u ms",
             (unsigned)delay_ms);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(
        runtime->reconnect_timer, (uint64_t)delay_ms * 1000));
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    vibe_wifi_runtime_t *runtime = (vibe_wifi_runtime_t *)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        runtime->reconnect_attempt = 0;
        return;
    }
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        atomic_store(&runtime->connected, false);
        runtime->ip[0] = '\0';
        if (runtime->config.deep_sleep_committed &&
            runtime->config.deep_sleep_committed(runtime->config.context)) {
            ESP_LOGI(TAG, "Wi-Fi stopped for deep sleep reason=%d",
                     disconnected ? disconnected->reason : -1);
            return;
        }
        if (runtime->profile_count > 1) {
            runtime->profile_retry_count++;
            bool no_ap =
                disconnected &&
                disconnected->reason == WIFI_REASON_NO_AP_FOUND;
            if (no_ap ||
                runtime->profile_retry_count >= WIFI_PROFILE_RETRY_LIMIT) {
                runtime->profile_index =
                    (runtime->profile_index + 1) % runtime->profile_count;
                runtime->profile_retry_count = 0;
                runtime->reconnect_attempt = 0;
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    apply_profile(runtime, runtime->profile_index));
            }
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d retry=%d profile=%u/%u",
                 disconnected ? disconnected->reason : -1,
                 runtime->profile_retry_count,
                 (unsigned)(runtime->profile_index + 1),
                 (unsigned)runtime->profile_count);
        schedule_reconnect(runtime);
        notify_status(runtime);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip =
            (const ip_event_got_ip_t *)event_data;
        atomic_store(&runtime->connected, true);
        if (got_ip) {
            snprintf(runtime->ip, sizeof(runtime->ip), IPSTR,
                     IP2STR(&got_ip->ip_info.ip));
        }
        runtime->profile_retry_count = 0;
        runtime->reconnect_attempt = 0;
        if (runtime->reconnect_timer &&
            esp_timer_is_active(runtime->reconnect_timer)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_timer_stop(runtime->reconnect_timer));
        }
        notify_status(runtime);
    }
}

esp_err_t vibe_wifi_runtime_init(vibe_wifi_runtime_t *runtime,
                                 const vibe_wifi_runtime_config_t *config)
{
    if (!runtime || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    atomic_init(&runtime->connected, false);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, runtime),
        TAG, "register Wi-Fi event");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, runtime),
        TAG, "register IP event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");

    const esp_timer_create_args_t timer_config = {
        .callback = reconnect_timer_cb,
        .arg = runtime,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&timer_config, &runtime->reconnect_timer),
        TAG, "wifi reconnect timer");
    ESP_RETURN_ON_ERROR(profiles_load(runtime), TAG, "load Wi-Fi profiles");

    bool profiles_changed = false;
    for (size_t index = 0; index < config->configured_profile_count; ++index) {
        profiles_changed |=
            profile_merge(runtime, &config->configured_profiles[index]);
    }
    wifi_config_t driver_config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &driver_config) == ESP_OK) {
        vibe_wifi_profile_t profile = {0};
        snprintf(profile.ssid, sizeof(profile.ssid), "%s",
                 (const char *)driver_config.sta.ssid);
        snprintf(profile.password, sizeof(profile.password), "%s",
                 (const char *)driver_config.sta.password);
        profiles_changed |= profile_merge(runtime, &profile);
    }
    if (profiles_changed) {
        ESP_RETURN_ON_ERROR(profiles_save(runtime), TAG,
                            "save Wi-Fi profiles");
    }
    if (runtime->profile_count == 0) {
        ESP_LOGW(TAG, "no Wi-Fi profiles configured; Wi-Fi disabled");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(apply_profile(runtime, runtime->profile_index),
                        TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    runtime->started = true;
    ESP_RETURN_ON_ERROR(configure_started_radio(runtime),
                        TAG, "wifi radio config");
    return esp_wifi_connect();
}

bool vibe_wifi_runtime_connected(const vibe_wifi_runtime_t *runtime)
{
    return runtime && atomic_load(&runtime->connected);
}

bool vibe_wifi_runtime_started(const vibe_wifi_runtime_t *runtime)
{
    return runtime && runtime->started;
}

void vibe_wifi_runtime_ip(const vibe_wifi_runtime_t *runtime,
                          char *ip,
                          size_t ip_len)
{
    if (ip && ip_len > 0) {
        snprintf(ip, ip_len, "%s", runtime ? runtime->ip : "");
    }
}

void vibe_wifi_runtime_ssid(const vibe_wifi_runtime_t *runtime,
                            char *ssid,
                            size_t ssid_len)
{
    if (!ssid || ssid_len == 0) {
        return;
    }
    ssid[0] = '\0';
    wifi_ap_record_t ap = {0};
    if (vibe_wifi_runtime_connected(runtime) &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(ssid, ssid_len, "%s", (const char *)ap.ssid);
    }
}

void vibe_wifi_runtime_bssid(const vibe_wifi_runtime_t *runtime,
                             char *bssid,
                             size_t bssid_len)
{
    if (!bssid || bssid_len == 0) {
        return;
    }
    bssid[0] = '\0';
    wifi_ap_record_t ap = {0};
    if (vibe_wifi_runtime_connected(runtime) &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(bssid, bssid_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
}

int vibe_wifi_runtime_rssi(const vibe_wifi_runtime_t *runtime)
{
    wifi_ap_record_t ap = {0};
    if (!vibe_wifi_runtime_connected(runtime) ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return VIBE_WIFI_RUNTIME_RSSI_UNKNOWN;
    }
    return ap.rssi;
}

bool vibe_wifi_runtime_current_profile(const vibe_wifi_runtime_t *runtime,
                                       vibe_wifi_profile_t *profile)
{
    if (!runtime || !profile ||
        runtime->profile_index >= runtime->profile_count) {
        return false;
    }
    vibe_wifi_profile_copy(profile, &runtime->profiles[runtime->profile_index]);
    return true;
}

esp_err_t vibe_wifi_runtime_store_profile(vibe_wifi_runtime_t *runtime,
                                          const vibe_wifi_profile_t *profile,
                                          size_t *profile_index)
{
    if (!runtime || !profile) {
        return ESP_ERR_INVALID_ARG;
    }
    vibe_wifi_profile_merge_result_t merge = vibe_wifi_profiles_merge(
        runtime->profiles, &runtime->profile_count,
        VIBE_WIFI_RUNTIME_MAX_PROFILES, profile);
    if (merge == VIBE_WIFI_PROFILE_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    if (merge == VIBE_WIFI_PROFILE_FULL) {
        size_t replace_index =
            runtime->profile_index < runtime->profile_count
                ? runtime->profile_index
                : 0;
        vibe_wifi_profile_copy(&runtime->profiles[replace_index], profile);
    }
    size_t found_index = 0;
    for (size_t index = 0; index < runtime->profile_count; ++index) {
        if (strcmp(runtime->profiles[index].ssid, profile->ssid) == 0) {
            found_index = index;
            break;
        }
    }
    ESP_RETURN_ON_ERROR(profiles_save(runtime), TAG,
                        "save Wi-Fi profiles");
    if (profile_index) {
        *profile_index = found_index;
    }
    return ESP_OK;
}

esp_err_t vibe_wifi_runtime_connect_profile(vibe_wifi_runtime_t *runtime,
                                            size_t profile_index)
{
    if (!runtime || profile_index >= runtime->profile_count) {
        return ESP_ERR_INVALID_ARG;
    }
    runtime->profile_index = profile_index;
    runtime->profile_retry_count = 0;
    runtime->reconnect_attempt = 0;
    if (!runtime->started) {
        ESP_RETURN_ON_ERROR(apply_profile(runtime, profile_index),
                            TAG, "apply Wi-Fi profile");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
        runtime->started = true;
        ESP_RETURN_ON_ERROR(configure_started_radio(runtime),
                            TAG, "Wi-Fi radio config");
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
        ESP_RETURN_ON_ERROR(apply_profile(runtime, profile_index),
                            TAG, "apply Wi-Fi profile");
    }
    return esp_wifi_connect();
}

esp_err_t vibe_wifi_runtime_set_performance(vibe_wifi_runtime_t *runtime,
                                            bool performance)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_wifi_set_ps(
        performance ? WIFI_PS_NONE : runtime->config.idle_power_save);
}

void vibe_wifi_runtime_reconnect_now(vibe_wifi_runtime_t *runtime)
{
    if (!runtime || vibe_wifi_runtime_connected(runtime) ||
        !runtime->reconnect_timer ||
        !esp_timer_is_active(runtime->reconnect_timer)) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(runtime->reconnect_timer));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(runtime->reconnect_timer, 1));
}

esp_err_t vibe_wifi_runtime_stop_for_sleep(vibe_wifi_runtime_t *runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->reconnect_timer &&
        esp_timer_is_active(runtime->reconnect_timer)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_stop(runtime->reconnect_timer));
    }
    atomic_store(&runtime->connected, false);
    runtime->ip[0] = '\0';
    return runtime->started ? esp_wifi_stop() : ESP_OK;
}
