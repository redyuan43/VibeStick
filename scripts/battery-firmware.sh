#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/battery-firmware.sh build <sticks3|stickc_plus_11|all>
  scripts/battery-firmware.sh flash <sticks3|stickc_plus_11> [serial-port|auto]
  scripts/battery-firmware.sh clean <sticks3|stickc_plus_11|all>

The script forces isolated sdkconfig/build directories:
  firmware/telemetry/<board>/sdkconfig.<board>
  firmware/telemetry/<board>/build.<board>
USAGE
}

if [[ $# -lt 2 ]]; then
  usage
  exit 2
fi

action="$1"
board="$2"
port="${3:-auto}"

if [[ "$board" == "all" ]]; then
  if [[ "$action" != "build" && "$action" != "clean" ]]; then
    usage
    exit 2
  fi
  "$0" "$action" sticks3
  "$0" "$action" stickc_plus_11
  exit 0
fi

case "$board" in
  sticks3)
    chip="esp32s3"
    project="firmware/telemetry/sticks3"
    flash_baud="${VIBE_STICK_FLASH_BAUD:-460800}"
    ;;
  stickc_plus_11)
    chip="esp32"
    project="firmware/telemetry/stickc_plus_11"
    flash_baud="${VIBE_STICK_FLASH_BAUD:-115200}"
    ;;
  *)
    usage
    exit 2
    ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Source ESP-IDF 5.5.1 export.sh first." >&2
  exit 127
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
project_abs="$repo_root/$project"
build_dir="$project_abs/build.$board"
sdkconfig="$project_abs/sdkconfig.$board"

idf() {
  idf.py -C "$project_abs" -B "$build_dir" -DSDKCONFIG="$sdkconfig" "$@"
}

detect_port() {
  local candidate
  local resolved
  local -a matches=()
  local -A seen=()
  shopt -s nullglob
  local -a candidates=(
    /dev/serial/by-id/*
    /dev/ttyACM*
    /dev/ttyUSB*
    /dev/cu.usbmodem*
    /dev/cu.usbserial*
  )
  shopt -u nullglob
  for candidate in "${candidates[@]}"; do
    [[ -e "$candidate" ]] || continue
    resolved="$(readlink -f "$candidate" 2>/dev/null || printf '%s' "$candidate")"
    [[ -z "${seen[$resolved]:-}" ]] || continue
    seen[$resolved]=1
    if python -m esptool --chip "$chip" --port "$candidate" chip_id >/dev/null 2>&1; then
      matches+=("$candidate")
    fi
  done
  if [[ ${#matches[@]} -ne 1 ]]; then
    echo "Expected exactly one $chip serial device, found ${#matches[@]}." >&2
    printf '  %s\n' "${matches[@]}" >&2
    echo "Pass an explicit serial port." >&2
    return 1
  fi
  printf '%s\n' "${matches[0]}"
}

case "$action" in
  build)
    echo "Building $board for chip $chip"
    idf set-target "$chip"
    idf build
    ;;
  flash)
    if [[ "$port" == "auto" ]]; then
      port="$(detect_port)"
    fi
    echo "Preparing flash for $board; expected chip is $chip"
    python -m esptool --chip "$chip" --port "$port" chip_id
    idf set-target "$chip"
    idf build
    idf -p "$port" -b "$flash_baud" flash
    ;;
  clean)
    echo "Cleaning isolated outputs for $board"
    rm -rf "$build_dir" "$sdkconfig" "$project_abs/sdkconfig.old"
    ;;
  *)
    usage
    exit 2
    ;;
esac
