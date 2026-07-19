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
  -o build-host-tests/test_vibe_policies
./build-host-tests/test_vibe_policies
