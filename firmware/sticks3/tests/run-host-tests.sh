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
  src/vibe_settings.c \
  src/vibe_bridge_profile_policy.c \
  src/vibe_wifi_policy.c \
  src/vibe_wav.c \
  -o build-host-tests/test_vibe_policies
./build-host-tests/test_vibe_policies

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_air_mouse.c \
  src/vibe_air_mouse.c \
  -lm \
  -o build-host-tests/test_vibe_air_mouse
./build-host-tests/test_vibe_air_mouse

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_input_router.c \
  src/vibe_input_router.c \
  -o build-host-tests/test_vibe_input_router
./build-host-tests/test_vibe_input_router

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_recording_controller.c \
  src/vibe_recording_controller.c \
  src/vibe_recording_policy.c \
  -o build-host-tests/test_vibe_recording_controller
./build-host-tests/test_vibe_recording_controller

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_motion_controller.c \
  src/vibe_motion_controller.c \
  -o build-host-tests/test_vibe_motion_controller
./build-host-tests/test_vibe_motion_controller

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_power_runtime.c \
  src/vibe_power_runtime.c \
  src/vibe_power_policy.c \
  -o build-host-tests/test_vibe_power_runtime
./build-host-tests/test_vibe_power_runtime

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_capture_profile.c \
  src/vibe_capture_profile.c \
  -o build-host-tests/test_vibe_capture_profile
./build-host-tests/test_vibe_capture_profile

cc -std=c11 -Wall -Wextra -Werror \
  -I include \
  tests/test_vibe_ima_adpcm.c \
  src/vibe_ima_adpcm.c \
  -o build-host-tests/test_vibe_ima_adpcm
./build-host-tests/test_vibe_ima_adpcm

CJSON_DIR="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
if [ -f "$CJSON_DIR/cJSON.c" ]; then
  cc -std=c11 -Wall -Wextra -Werror \
    -I include \
    -I "$CJSON_DIR" \
    tests/test_vibe_state.c \
    src/vibe_app_state.c \
    src/vibe_state_json.c \
    "$CJSON_DIR/cJSON.c" \
    -lm \
    -o build-host-tests/test_vibe_state
elif pkg-config --exists libcjson; then
  cc -std=c11 -Wall -Wextra -Werror \
    -I include \
    $(pkg-config --cflags libcjson) \
    tests/test_vibe_state.c \
    src/vibe_app_state.c \
    src/vibe_state_json.c \
    $(pkg-config --libs libcjson) \
    -o build-host-tests/test_vibe_state
else
  printf '%s\n' "cJSON development files were not found." >&2
  exit 1
fi
./build-host-tests/test_vibe_state
