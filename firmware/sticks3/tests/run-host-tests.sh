#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build-host-tests
cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_policies.c \
  src/vibe_ota_policy.c \
  src/vibe_recording_policy.c \
  src/vibe_power_policy.c \
  src/vibe_bridge_profile_policy.c \
  src/vibe_wifi_policy.c \
  src/vibe_wav.c \
  -o build-host-tests/test_vibe_policies
./build-host-tests/test_vibe_policies

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  -I generated \
  tests/test_vibe_bt_ui_renderer.c \
  src/vibe_bt_ui_renderer.c \
  generated/vibe_minijoy_pet_assets.c \
  -o build-host-tests/test_vibe_bt_ui_renderer
./build-host-tests/test_vibe_bt_ui_renderer

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_audio_pdm_filter.c \
  src/vibe_audio_pdm_filter.c \
  -o build-host-tests/test_vibe_audio_pdm_filter
./build-host-tests/test_vibe_audio_pdm_filter
