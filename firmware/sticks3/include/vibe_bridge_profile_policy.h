#pragma once

#include <stdbool.h>
#include <stddef.h>

bool vibe_bridge_health_name_supported(const char *bridge_name);
bool vibe_bridge_identity_is_generic(const char *bridge_id);
void vibe_bridge_fallback_id(const char *host, char *id, size_t id_len);
