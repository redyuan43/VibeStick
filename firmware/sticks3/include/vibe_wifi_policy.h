#pragma once

#include <stddef.h>
#include <stdint.h>

#define VIBE_WIFI_PROFILE_SSID_LEN 33
#define VIBE_WIFI_PROFILE_PASSWORD_LEN 65

typedef struct {
    char ssid[VIBE_WIFI_PROFILE_SSID_LEN];
    char password[VIBE_WIFI_PROFILE_PASSWORD_LEN];
} vibe_wifi_profile_t;

typedef enum {
    VIBE_WIFI_PROFILE_INVALID = 0,
    VIBE_WIFI_PROFILE_UNCHANGED,
    VIBE_WIFI_PROFILE_ADDED,
    VIBE_WIFI_PROFILE_UPDATED,
    VIBE_WIFI_PROFILE_FULL,
} vibe_wifi_profile_merge_result_t;

void vibe_wifi_profile_copy(vibe_wifi_profile_t *dest,
                            const vibe_wifi_profile_t *source);

vibe_wifi_profile_merge_result_t vibe_wifi_profiles_merge(
    vibe_wifi_profile_t *profiles,
    size_t *count,
    size_t capacity,
    const vibe_wifi_profile_t *candidate);

uint32_t vibe_wifi_reconnect_delay_ms(unsigned int attempt,
                                      uint32_t max_delay_ms);
