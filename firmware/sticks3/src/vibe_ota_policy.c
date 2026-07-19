#include "vibe_ota_policy.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

bool vibe_ota_parse_semantic_version(const char *raw, uint32_t version[3])
{
    if (!raw || !version) {
        return false;
    }

    const char *cursor = raw;
    if (*cursor == 'v' || *cursor == 'V') {
        cursor++;
    }
    for (size_t index = 0; index < 3; index++) {
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || value > UINT32_MAX) {
            return false;
        }
        version[index] = (uint32_t)value;
        if (index < 2) {
            if (*end != '.') {
                return false;
            }
            cursor = end + 1;
        } else if (*end != '\0' && *end != '-' && *end != '+') {
            return false;
        }
    }
    return true;
}

bool vibe_ota_compare_semantic_versions(const char *candidate,
                                        const char *current,
                                        int *comparison)
{
    uint32_t candidate_version[3] = {0};
    uint32_t current_version[3] = {0};
    if (!comparison ||
        !vibe_ota_parse_semantic_version(candidate, candidate_version) ||
        !vibe_ota_parse_semantic_version(current, current_version)) {
        return false;
    }
    for (size_t index = 0; index < 3; index++) {
        if (candidate_version[index] < current_version[index]) {
            *comparison = -1;
            return true;
        }
        if (candidate_version[index] > current_version[index]) {
            *comparison = 1;
            return true;
        }
    }
    *comparison = 0;
    return true;
}

bool vibe_ota_version_is_newer(const char *candidate, const char *current)
{
    int comparison = 0;
    return vibe_ota_compare_semantic_versions(candidate, current, &comparison) &&
           comparison > 0;
}
