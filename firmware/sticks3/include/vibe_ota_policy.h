#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool available;
    char board[24];
    char version[48];
    char build_id[64];
    char sha256[65];
    char elf_sha256[65];
    char url[160];
    int size;
} vibe_ota_manifest_t;

typedef enum {
    VIBE_OTA_DECISION_UNAVAILABLE,
    VIBE_OTA_DECISION_BOARD_MISMATCH,
    VIBE_OTA_DECISION_INVALID_VERSION,
    VIBE_OTA_DECISION_OLDER_VERSION,
    VIBE_OTA_DECISION_CURRENT_VERSION,
    VIBE_OTA_DECISION_CURRENT_IMAGE_SHA256,
    VIBE_OTA_DECISION_CURRENT_ELF_SHA256,
    VIBE_OTA_DECISION_MISSING_IDENTITY,
    VIBE_OTA_DECISION_CURRENT_BUILD,
    VIBE_OTA_DECISION_UPDATE,
} vibe_ota_decision_t;

bool vibe_ota_parse_semantic_version(const char *raw, uint32_t version[3]);
bool vibe_ota_compare_semantic_versions(const char *candidate,
                                        const char *current,
                                        int *comparison);
bool vibe_ota_version_is_newer(const char *candidate, const char *current);
vibe_ota_decision_t vibe_ota_update_decision(const vibe_ota_manifest_t *manifest,
                                              const char *expected_board,
                                              const char *current_version,
                                              const char *current_build_id,
                                              const char *running_image_sha256,
                                              const char *running_elf_sha256);
