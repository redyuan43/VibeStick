#pragma once

#include <stdbool.h>
#include <stdint.h>

bool vibe_ota_parse_semantic_version(const char *raw, uint32_t version[3]);
bool vibe_ota_compare_semantic_versions(const char *candidate,
                                        const char *current,
                                        int *comparison);
bool vibe_ota_version_is_newer(const char *candidate, const char *current);
