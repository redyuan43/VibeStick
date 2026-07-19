#include "vibe_ota_policy.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

vibe_ota_decision_t vibe_ota_update_decision(const vibe_ota_manifest_t *manifest,
                                              const char *expected_board,
                                              const char *current_version,
                                              const char *current_build_id,
                                              const char *running_image_sha256,
                                              const char *running_elf_sha256)
{
    if (!manifest || !manifest->available) {
        return VIBE_OTA_DECISION_UNAVAILABLE;
    }
    if (!expected_board || strcmp(manifest->board, expected_board) != 0) {
        return VIBE_OTA_DECISION_BOARD_MISMATCH;
    }
    int comparison = 0;
    if (!vibe_ota_compare_semantic_versions(manifest->version, current_version, &comparison)) {
        return VIBE_OTA_DECISION_INVALID_VERSION;
    }
    if (comparison < 0) {
        return VIBE_OTA_DECISION_OLDER_VERSION;
    }
    if (comparison == 0) {
        return VIBE_OTA_DECISION_CURRENT_VERSION;
    }
    if (manifest->sha256[0] != '\0' && running_image_sha256 && running_image_sha256[0] != '\0') {
        return strcmp(manifest->sha256, running_image_sha256) == 0
                   ? VIBE_OTA_DECISION_CURRENT_IMAGE_SHA256
                   : VIBE_OTA_DECISION_UPDATE;
    }
    if (manifest->elf_sha256[0] != '\0') {
        size_t running_len = running_elf_sha256 ? strlen(running_elf_sha256) : 0;
        if (running_len > 0 && strncmp(manifest->elf_sha256, running_elf_sha256, running_len) == 0) {
            return VIBE_OTA_DECISION_CURRENT_ELF_SHA256;
        }
        return VIBE_OTA_DECISION_UPDATE;
    }
    if (manifest->build_id[0] == '\0') {
        return VIBE_OTA_DECISION_MISSING_IDENTITY;
    }
    return current_build_id && strcmp(manifest->build_id, current_build_id) == 0
               ? VIBE_OTA_DECISION_CURRENT_BUILD
               : VIBE_OTA_DECISION_UPDATE;
}
