#pragma once

#if __has_include("vibe_telemetry_secrets.h")
#include "vibe_telemetry_secrets.h"
#else
#define VIBE_TELEMETRY_WIFI_SSID ""
#define VIBE_TELEMETRY_WIFI_PASSWORD ""
#define VIBE_TELEMETRY_BASE_URL "http://127.0.0.1:8765"
#define VIBE_TELEMETRY_SHARED_SECRET "development-placeholder"
#endif
