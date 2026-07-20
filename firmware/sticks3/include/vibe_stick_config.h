#pragma once

#define VIBE_STICK_DEVICE_NAME "VibeStick"
#define FIRMWARE_NAME "vibestick"
#define VIBE_STICK_FIRMWARE_VERSION_STICKS3 "0.1.50"
#define VIBE_STICK_FIRMWARE_VERSION_STICKC_PLUS "0.1.37"
#if defined(VIBE_BOARD_STICKS3)
#define FIRMWARE_VERSION VIBE_STICK_FIRMWARE_VERSION_STICKS3
#else
#define FIRMWARE_VERSION VIBE_STICK_FIRMWARE_VERSION_STICKC_PLUS
#endif
#define TRANSPORT "HTTP"
#define VIBE_STICK_STATE_PATH "/state"
#define VIBE_STICK_EVENT_PATH "/event"
#define VIBE_STICK_QUOTA_REFRESH_PATH "/quota/refresh"
#define VIBE_STICK_RECORDING_START_PATH "/recording/start"
#define VIBE_STICK_RECORDING_AUDIO_PATH "/recording/audio"
#define VIBE_STICK_RECORDING_STOP_PATH "/recording/stop"
#define VIBE_STICK_RECORDING_TTS_PATH "/recording/tts"
#define VIBE_STICK_OTA_MANIFEST_PATH "/ota/manifest"
#define VIBE_STICK_OTA_BIN_PATH "/ota/bin"
#define VIBE_STICK_STATE_POLL_MS 2000

#if __has_include("vibe_stick_secrets.h")
#include "vibe_stick_secrets.h"
#else
#define VIBE_STICK_WIFI_SSID ""
#define VIBE_STICK_WIFI_PASSWORD ""
#define VIBE_STICK_BRIDGE_HOST "127.0.0.1"
#define VIBE_STICK_BRIDGE_PORT 8765
#endif

#ifndef VIBE_STICK_BRIDGE_TOKEN
#define VIBE_STICK_BRIDGE_TOKEN ""
#endif
#ifndef VIBE_STICK_BRIDGE_ID
#define VIBE_STICK_BRIDGE_ID "capswriter-m5-voice-bridge"
#endif
#ifndef VIBE_STICK_BRIDGE_LABEL
#define VIBE_STICK_BRIDGE_LABEL "CapsWriter"
#endif
#define VIBE_STICK_BRIDGE_PROFILE_MAX_COUNT 8
