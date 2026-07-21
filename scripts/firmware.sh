#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
FIRMWARE_DIR="$ROOT_DIR/firmware/sticks3"

usage() {
  printf '%s\n' "Usage: scripts/firmware.sh <sticks3|stickc_plus|stickc_plus_se|stickc_plus_minijoy_bt> <build|flash|monitor|flash monitor|...> [idf.py args]" >&2
}

if [ "$#" -lt 2 ]; then
  usage
  exit 2
fi

BOARD="$1"
shift

case "$BOARD" in
  sticks3|stickc_plus|stickc_plus_se|stickc_plus_minijoy_bt)
    ;;
  *)
    usage
    exit 2
    ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
  printf '%s\n' "idf.py was not found on PATH. Source ESP-IDF export.sh first:" >&2
  printf '%s\n' ". \$HOME/esp/esp-idf/export.sh" >&2
  exit 127
fi

cd "$FIRMWARE_DIR"
exec idf.py \
  -B "build-$BOARD" \
  -DSDKCONFIG="sdkconfig.$BOARD" \
  -DVIBE_BOARD="$BOARD" \
  "$@"
