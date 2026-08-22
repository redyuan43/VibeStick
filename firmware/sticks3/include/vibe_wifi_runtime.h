#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "vibe_wifi_policy.h"

#define VIBE_WIFI_RUNTIME_MAX_PROFILES 4
#define VIBE_WIFI_RUNTIME_RSSI_UNKNOWN -127

typedef bool (*vibe_wifi_runtime_bool_fn)(void *context);
typedef void (*vibe_wifi_runtime_status_fn)(bool connected,
                                            const char *ip,
                                            void *context);

typedef struct {
    const vibe_wifi_profile_t *configured_profiles;
    size_t configured_profile_count;
    wifi_ps_type_t idle_power_save;
    int max_tx_power;
    vibe_wifi_runtime_bool_fn deep_sleep_committed;
    vibe_wifi_runtime_status_fn status_changed;
    void *context;
} vibe_wifi_runtime_config_t;

typedef struct {
    vibe_wifi_runtime_config_t config;
    vibe_wifi_profile_t profiles[VIBE_WIFI_RUNTIME_MAX_PROFILES];
    size_t profile_count;
    size_t profile_index;
    int profile_retry_count;
    unsigned int reconnect_attempt;
    esp_timer_handle_t reconnect_timer;
    atomic_bool connected;
    bool started;
    char ip[16];
} vibe_wifi_runtime_t;

esp_err_t vibe_wifi_runtime_init(vibe_wifi_runtime_t *runtime,
                                 const vibe_wifi_runtime_config_t *config);
bool vibe_wifi_runtime_connected(const vibe_wifi_runtime_t *runtime);
bool vibe_wifi_runtime_started(const vibe_wifi_runtime_t *runtime);
void vibe_wifi_runtime_ip(const vibe_wifi_runtime_t *runtime,
                          char *ip,
                          size_t ip_len);
void vibe_wifi_runtime_ssid(const vibe_wifi_runtime_t *runtime,
                            char *ssid,
                            size_t ssid_len);
void vibe_wifi_runtime_bssid(const vibe_wifi_runtime_t *runtime,
                             char *bssid,
                             size_t bssid_len);
int vibe_wifi_runtime_rssi(const vibe_wifi_runtime_t *runtime);
bool vibe_wifi_runtime_current_profile(const vibe_wifi_runtime_t *runtime,
                                       vibe_wifi_profile_t *profile);
esp_err_t vibe_wifi_runtime_store_profile(vibe_wifi_runtime_t *runtime,
                                          const vibe_wifi_profile_t *profile,
                                          size_t *profile_index);
esp_err_t vibe_wifi_runtime_connect_profile(vibe_wifi_runtime_t *runtime,
                                            size_t profile_index);
esp_err_t vibe_wifi_runtime_set_performance(vibe_wifi_runtime_t *runtime,
                                            bool performance);
void vibe_wifi_runtime_reconnect_now(vibe_wifi_runtime_t *runtime);
esp_err_t vibe_wifi_runtime_stop_for_sleep(vibe_wifi_runtime_t *runtime);

