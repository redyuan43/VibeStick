#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BOARDS=(sticks3 stickc_plus stickc_plus_se cardputer_adv)

if ! command -v idf.py >/dev/null 2>&1; then
  printf '%s\n' "idf.py was not found. Source ESP-IDF export.sh first." >&2
  exit 127
fi

for board in "${BOARDS[@]}"; do
  printf '\n== Building %s ==\n' "$board"
  "$ROOT_DIR/scripts/firmware.sh" "$board" build
  image="$ROOT_DIR/firmware/sticks3/build-$board/vibe_stick_$board.bin"
  printf '%s %s bytes\n' "$board" "$(wc -c < "$image")"
  python3 "$ROOT_DIR/scripts/check-firmware-size.py" "$board"
done
