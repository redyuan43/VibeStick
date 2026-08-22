#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SUPPORTED_BOARDS=(sticks3 stickc_plus stickc_plus_se cardputer_adv)
BOARDS=()
VERIFY_ARGS=()

usage() {
  printf '%s\n' \
    "Usage: scripts/release-firmware.sh [--local-only] [--base-url URL] <board...|all>" >&2
}

board_supported() {
  local candidate="$1"
  local board
  for board in "${SUPPORTED_BOARDS[@]}"; do
    [[ "$candidate" == "$board" ]] && return 0
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --local-only)
      VERIFY_ARGS+=("$1")
      shift
      ;;
    --base-url|--timeout)
      [[ $# -ge 2 ]] || {
        usage
        exit 2
      }
      VERIFY_ARGS+=("$1" "$2")
      shift 2
      ;;
    all)
      BOARDS=("${SUPPORTED_BOARDS[@]}")
      shift
      ;;
    *)
      if ! board_supported "$1"; then
        usage
        exit 2
      fi
      BOARDS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#BOARDS[@]} -eq 0 ]]; then
  usage
  exit 2
fi

if ! command -v idf.py >/dev/null 2>&1; then
  printf '%s\n' "idf.py was not found. Source ESP-IDF export.sh first." >&2
  exit 127
fi

for board in "${BOARDS[@]}"; do
  "$ROOT_DIR/scripts/firmware.sh" "$board" build
  python3 "$ROOT_DIR/scripts/check-firmware-size.py" "$board"
  "$ROOT_DIR/scripts/ota_publish.sh" "$board"
done

exec python3 "$ROOT_DIR/scripts/verify-ota-release.py" \
  "${VERIFY_ARGS[@]}" "${BOARDS[@]}"
