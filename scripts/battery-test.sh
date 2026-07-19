#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_PID=""
TELEMETRY_PORT="${VIBE_STICK_TELEMETRY_PORT:-8878}"
TELEMETRY_LOCAL_URL="${VIBE_STICK_TELEMETRY_URL:-http://127.0.0.1:$TELEMETRY_PORT}"

cleanup() {
  if [[ -n "$BRIDGE_PID" ]]; then
    kill "$BRIDGE_PID" >/dev/null 2>&1 || true
    wait "$BRIDGE_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ -f "$ROOT_DIR/.env" ]; then
  set -a
  # shellcheck disable=SC1091
  . "$ROOT_DIR/.env"
  set +a
fi

ensure_bridge() {
  health="$(curl -fsS "$TELEMETRY_LOCAL_URL/health" 2>/dev/null || true)"
  if [[ "$health" == *"battery_telemetry_v1"* ]]; then
    return
  fi
  if [[ -n "$health" ]]; then
    echo "$TELEMETRY_LOCAL_URL is running a bridge without battery telemetry support." >&2
    echo "Stop or reinstall that bridge before starting a battery test." >&2
    exit 1
  fi
  PYTHONPATH="$ROOT_DIR/bridge/src" \
    python3 -m vibe_stick.telemetry.server --host 0.0.0.0 --port "$TELEMETRY_PORT" \
    >"${TMPDIR:-/tmp}/vibestick-telemetry.log" 2>&1 &
  BRIDGE_PID=$!
  for _ in {1..50}; do
    health="$(curl -fsS "$TELEMETRY_LOCAL_URL/health" 2>/dev/null || true)"
    if [[ "$health" == *"battery_telemetry_v1"* ]]; then
      return
    fi
    sleep 0.2
  done
  echo "Battery telemetry bridge did not become ready." >&2
  exit 1
}

args=("$@")
if [[ ${#args[@]} -gt 0 && ( "${args[0]}" == "smoke" || "${args[0]}" == "full" || "${args[0]}" == "charge" ) ]]; then
  mode="${args[0]}"
  board=""
  port="auto"
  resume_unplugged="false"
  forwarded=()
  index=0
  while [[ $index -lt ${#args[@]} ]]; do
    case "${args[$index]}" in
      --board)
        board="${args[$((index + 1))]:-}"
        forwarded+=("--board" "$board")
        index=$((index + 2))
        ;;
      --port)
        port="${args[$((index + 1))]:-auto}"
        index=$((index + 2))
        ;;
      --resume-unplugged)
        resume_unplugged="true"
        forwarded+=("${args[$index]}")
        index=$((index + 1))
        ;;
      *)
        forwarded+=("${args[$index]}")
        index=$((index + 1))
        ;;
    esac
  done
  ensure_bridge
  if [[ "$mode" != "charge" && "$resume_unplugged" != "true" && -n "$board" ]]; then
    "$ROOT_DIR/scripts/battery-firmware.sh" flash "$board" "$port"
  fi
  args=("${forwarded[@]}")
fi

PYTHONPATH="$ROOT_DIR/bridge/src${PYTHONPATH:+:$PYTHONPATH}" \
  python3 -m vibe_stick.telemetry.cli --base-url "$TELEMETRY_LOCAL_URL" "${args[@]}"
