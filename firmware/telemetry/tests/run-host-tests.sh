#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build-host-tests
cc -std=c11 -Wall -Wextra -Werror \
  -I components/vibe_telemetry_common/include \
  tests/test_telemetry_common.c \
  components/vibe_telemetry_common/src/vibe_telemetry_json.c \
  -o build-host-tests/test_telemetry_common
./build-host-tests/test_telemetry_common

cc -std=c11 -Wall -Wextra -Werror \
  -I ../shared \
  tests/test_battery_curve.c \
  ../shared/sticks3_battery_curve.c \
  -o build-host-tests/test_battery_curve
./build-host-tests/test_battery_curve
