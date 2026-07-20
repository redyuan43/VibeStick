#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TELEMETRY_PORT="${VIBE_STICK_TELEMETRY_PORT:-8878}"
cd "$ROOT_DIR/bridge"

if [ -f "$ROOT_DIR/.env" ]; then
  set -a
  . "$ROOT_DIR/.env"
  set +a
fi

token="${VIBE_STICK_TELEMETRY_TOKEN:-${VIBE_STICK_BRIDGE_TOKEN:-}}"
case "$token" in
  ""|change-this-shared-token|paste-generated-token-here|changeme|change-me)
    printf '%s\n' "A telemetry token is required because dev.sh exposes port $TELEMETRY_PORT on 0.0.0.0." >&2
    printf '%s\n' "Generate one with: openssl rand -hex 32" >&2
    exit 1
    ;;
esac

PYTHONPATH="$ROOT_DIR/bridge/src" exec python3 -m vibe_stick.telemetry.server \
  --host 0.0.0.0 --port "$TELEMETRY_PORT"
