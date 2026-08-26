#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/vibestick-cardputer-minimal-tests"
mkdir -p "$BUILD_DIR"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT_DIR/include" \
  "$ROOT_DIR/tests/test_vibe_ima_adpcm.c" \
  "$ROOT_DIR/src/vibe_ima_adpcm.c" \
  -o "$BUILD_DIR/test_vibe_ima_adpcm"
"$BUILD_DIR/test_vibe_ima_adpcm"
