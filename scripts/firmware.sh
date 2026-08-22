#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
FIRMWARE_DIR="$ROOT_DIR/firmware/sticks3"

usage() {
  printf '%s\n' "Usage: scripts/firmware.sh <sticks3|cardputer_adv|stickc_plus|stickc_plus_se> <build|flash|monitor|flash monitor|...> [idf.py args]" >&2
}

if [ "$#" -lt 2 ]; then
  usage
  exit 2
fi

BOARD="$1"
shift

case "$BOARD" in
  sticks3|cardputer_adv|stickc_plus|stickc_plus_se)
    ;;
  *)
    usage
    exit 2
    ;;
esac

FLASH_REQUESTED=0
FLASH_BAUD=""
EXPECT_BAUD_VALUE=0
for arg in "$@"; do
  if [ "$EXPECT_BAUD_VALUE" -eq 1 ]; then
    FLASH_BAUD="$arg"
    EXPECT_BAUD_VALUE=0
    continue
  fi
  case "$arg" in
    *flash*)
      FLASH_REQUESTED=1
      ;;
    -b|--baud)
      EXPECT_BAUD_VALUE=1
      ;;
    -b?*)
      FLASH_BAUD="${arg#-b}"
      ;;
    --baud=*)
      FLASH_BAUD="${arg#--baud=}"
      ;;
  esac
done
if [ "$EXPECT_BAUD_VALUE" -eq 1 ]; then
  printf '%s\n' "Missing value after baud option." >&2
  exit 2
fi
if [ "$FLASH_REQUESTED" -eq 1 ]; then
  if [ -n "$FLASH_BAUD" ] && [ "$FLASH_BAUD" != "115200" ]; then
    printf '%s\n' "Firmware flashing is fixed at 115200 baud for all boards." >&2
    exit 2
  fi
  if [ -z "$FLASH_BAUD" ]; then
    set -- -b 115200 "$@"
  fi
fi

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
