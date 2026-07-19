#include "vibe_bridge_profile_policy.h"

#include <string.h>

bool vibe_bridge_health_name_supported(const char *bridge_name)
{
    return bridge_name &&
           (strcmp(bridge_name, "vibestick-bridge") == 0 ||
            strcmp(bridge_name, "capswriter-m5-voice-bridge") == 0);
}

bool vibe_bridge_identity_is_generic(const char *bridge_id)
{
    return !bridge_id || bridge_id[0] == '\0' ||
           strcmp(bridge_id, "vibestick-bridge") == 0 ||
           strcmp(bridge_id, "capswriter-m5-voice-bridge") == 0;
}

void vibe_bridge_fallback_id(const char *host, char *id, size_t id_len)
{
    if (!id || id_len == 0) {
        return;
    }
    const char *source = host && host[0] != '\0' ? host : "bridge";
    size_t used = 0;
    const char prefix[] = "lan-";
    for (size_t index = 0; index < sizeof(prefix) - 1 && used + 1 < id_len; index++) {
        id[used++] = prefix[index];
    }
    for (size_t index = 0; source[index] != '\0' && used + 1 < id_len; index++) {
        id[used++] = source[index] == '.' ? '-' : source[index];
    }
    id[used] = '\0';
}
