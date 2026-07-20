#include "vibe_wifi_policy.h"

#include <string.h>

static void copy_string(char *dest, size_t dest_len, const char *source)
{
    if (!dest || dest_len == 0) {
        return;
    }
    const char *value = source ? source : "";
    size_t length = 0;
    while (length + 1 < dest_len && value[length] != '\0') {
        length++;
    }
    memcpy(dest, value, length);
    dest[length] = '\0';
}

void vibe_wifi_profile_copy(vibe_wifi_profile_t *dest,
                            const vibe_wifi_profile_t *source)
{
    if (!dest || !source) {
        return;
    }
    copy_string(dest->ssid, sizeof(dest->ssid), source->ssid);
    copy_string(dest->password, sizeof(dest->password), source->password);
}

vibe_wifi_profile_merge_result_t vibe_wifi_profiles_merge(
    vibe_wifi_profile_t *profiles,
    size_t *count,
    size_t capacity,
    const vibe_wifi_profile_t *candidate)
{
    if (!profiles || !count || !candidate || candidate->ssid[0] == '\0' ||
        *count > capacity) {
        return VIBE_WIFI_PROFILE_INVALID;
    }

    for (size_t index = 0; index < *count; index++) {
        if (strcmp(profiles[index].ssid, candidate->ssid) != 0) {
            continue;
        }
        if (strcmp(profiles[index].password, candidate->password) == 0) {
            return VIBE_WIFI_PROFILE_UNCHANGED;
        }
        vibe_wifi_profile_copy(&profiles[index], candidate);
        return VIBE_WIFI_PROFILE_UPDATED;
    }

    if (*count >= capacity) {
        return VIBE_WIFI_PROFILE_FULL;
    }
    vibe_wifi_profile_copy(&profiles[*count], candidate);
    (*count)++;
    return VIBE_WIFI_PROFILE_ADDED;
}

uint32_t vibe_wifi_reconnect_delay_ms(unsigned int attempt,
                                      uint32_t max_delay_ms)
{
    static const uint32_t delays_ms[] = {1000, 2000, 4000, 8000, 30000};
    size_t index = attempt;
    if (index >= sizeof(delays_ms) / sizeof(delays_ms[0])) {
        index = sizeof(delays_ms) / sizeof(delays_ms[0]) - 1;
    }
    return delays_ms[index] > max_delay_ms ? max_delay_ms : delays_ms[index];
}
