#pragma once

// Copy this file to vibe_stick_secrets.h and fill in local values.
// vibe_stick_secrets.h is intentionally ignored by git.

#define VIBE_STICK_WIFI_SSID "your-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"

// Optional: keep multiple known 2.4 GHz networks on the device.
// The firmware stores these profiles in NVS, so normal OTA updates keep them.
#define VIBE_STICK_WIFI_PROFILES \
    { \
        { VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD }, \
        { "your-second-wifi", "your-second-password" }, \
    }

#define VIBE_STICK_BRIDGE_HOST "192.168.1.10"
#define VIBE_STICK_BRIDGE_PORT 8765
#define VIBE_STICK_BRIDGE_TOKEN "paste-generated-token-here"
