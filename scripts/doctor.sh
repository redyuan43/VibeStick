#!/usr/bin/env sh
set -u

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ENV_PATH="$ROOT_DIR/.env"
SECRETS_PATH="$ROOT_DIR/firmware/sticks3/include/vibe_stick_secrets.h"
TELEMETRY_SECRETS_PATH="$ROOT_DIR/firmware/telemetry/secrets/vibe_telemetry_secrets.h"
APP_SUPPORT_DIR="$HOME/Library/Application Support/VibeStick"

PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf 'PASS %s\n' "$1"
}

warn() {
  WARN_COUNT=$((WARN_COUNT + 1))
  printf 'WARN %s\n' "$1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf 'FAIL %s\n' "$1"
}

env_value() {
  key="$1"
  file="$2"
  [ -f "$file" ] || return 0
  awk -F= -v key="$key" '
    /^[[:space:]]*#/ { next }
    {
      k = $1
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", k)
      if (k == key) {
        sub(/^[^=]*=/, "")
        gsub(/^[[:space:]]+|[[:space:]]+$/, "")
        gsub(/^"/, "")
        gsub(/"$/, "")
        print
        exit
      }
    }
  ' "$file"
}

secret_value() {
  key="$1"
  file="$2"
  [ -f "$file" ] || return 0
  awk -v key="$key" '
    $1 == "#define" && $2 == key {
      value = $0
      sub(/^[^"]*"/, "", value)
      sub(/".*$/, "", value)
      print value
      exit
    }
  ' "$file"
}

is_placeholder_token() {
  case "${1:-}" in
    ""|change-this-shared-token|paste-generated-token-here|changeme|change-me|your-token)
      return 0
      ;;
  esac
  return 1
}

is_placeholder_wifi() {
  case "${1:-}" in
    ""|your-wifi|YOUR_WIFI_SSID|ssid|wifi-ssid)
      return 0
      ;;
  esac
  return 1
}

is_placeholder_password() {
  case "${1:-}" in
    ""|your-password|YOUR_WIFI_PASSWORD|password|wifi-password)
      return 0
      ;;
  esac
  return 1
}

is_placeholder_host() {
  case "${1:-}" in
    ""|127.0.0.1|0.0.0.0|192.168.1.10|YOUR_MAC_IP|your-mac-ip)
      return 0
      ;;
  esac
  return 1
}

check_python() {
  if ! command -v python3 >/dev/null 2>&1; then
    fail "Python 3 is not installed or not on PATH."
    return
  fi
  if python3 - <<'PY' >/dev/null 2>&1
import sys
raise SystemExit(0 if sys.version_info >= (3, 11) else 1)
PY
  then
    pass "Python >= 3.11 is available."
  else
    fail "Python >= 3.11 is required."
  fi
}

check_esp_idf() {
  if command -v idf.py >/dev/null 2>&1; then
    pass "ESP-IDF is available on PATH."
    return
  fi
  export_path=""
  if [ -d "$HOME/esp" ]; then
    export_path="$(find "$HOME/esp" -path '*/esp-idf/export.sh' -print -quit 2>/dev/null || true)"
  fi
  if [ -n "$export_path" ]; then
    pass "ESP-IDF export script was found."
  else
    warn "ESP-IDF was not found; only firmware build/flash needs it."
  fi
}

check_dotenv() {
  if [ -f "$ENV_PATH" ]; then
    pass ".env exists."
  else
    fail ".env is missing; run scripts/setup.sh."
    return
  fi

  env_token="$(env_value VIBE_STICK_BRIDGE_TOKEN "$ENV_PATH")"
  if is_placeholder_token "$env_token"; then
    warn "VIBE_STICK_BRIDGE_TOKEN is empty or placeholder; binding 0.0.0.0 would be unsafe and install/dev scripts will refuse it."
  else
    pass "VIBE_STICK_BRIDGE_TOKEN is set in .env."
  fi
}

check_secrets() {
  if [ -f "$SECRETS_PATH" ]; then
    pass "firmware secrets file exists."
  else
    fail "firmware secrets file is missing; run scripts/setup.sh."
    return
  fi

  wifi_ssid="$(secret_value VIBE_STICK_WIFI_SSID "$SECRETS_PATH")"
  wifi_password="$(secret_value VIBE_STICK_WIFI_PASSWORD "$SECRETS_PATH")"
  bridge_host="$(secret_value VIBE_STICK_BRIDGE_HOST "$SECRETS_PATH")"

  if is_placeholder_wifi "$wifi_ssid"; then
    fail "Wi-Fi SSID is empty or placeholder in firmware secrets."
  else
    pass "Wi-Fi SSID is configured in firmware secrets."
  fi

  if is_placeholder_password "$wifi_password"; then
    fail "Wi-Fi password is empty or placeholder in firmware secrets."
  else
    pass "Wi-Fi password is configured in firmware secrets."
  fi

  if is_placeholder_host "$bridge_host"; then
    fail "VIBE_STICK_BRIDGE_HOST is empty, loopback, wildcard, or example placeholder."
  else
    pass "VIBE_STICK_BRIDGE_HOST is configured in firmware secrets."
    lan_ip="$(ipconfig getifaddr en0 2>/dev/null || true)"
    if [ -n "$lan_ip" ] && [ "$bridge_host" != "$lan_ip" ]; then
      warn "VIBE_STICK_BRIDGE_HOST ($bridge_host) does not match detected en0 IP ($lan_ip). Keep it only if the bridge runs at that address."
    fi
  fi
}

check_token_match() {
  [ -f "$ENV_PATH" ] && [ -f "$SECRETS_PATH" ] || return
  env_token="$(env_value VIBE_STICK_BRIDGE_TOKEN "$ENV_PATH")"
  secret_token="$(secret_value VIBE_STICK_BRIDGE_TOKEN "$SECRETS_PATH")"
  if [ "$env_token" = "$secret_token" ]; then
    if is_placeholder_token "$env_token"; then
      warn "Bridge tokens match but are empty or placeholder."
    else
      pass "Bridge token matches between .env and firmware secrets."
    fi
  else
    fail "Bridge token differs between .env and firmware secrets; button POST requests will get 401."
  fi
}

check_telemetry_secrets() {
  if [ ! -f "$TELEMETRY_SECRETS_PATH" ]; then
    warn "Battery telemetry firmware secrets are missing; run scripts/setup.sh."
    return
  fi

  telemetry_ssid="$(secret_value VIBE_TELEMETRY_WIFI_SSID "$TELEMETRY_SECRETS_PATH")"
  telemetry_password="$(secret_value VIBE_TELEMETRY_WIFI_PASSWORD "$TELEMETRY_SECRETS_PATH")"
  telemetry_url="$(secret_value VIBE_TELEMETRY_BASE_URL "$TELEMETRY_SECRETS_PATH")"
  telemetry_token="$(secret_value VIBE_TELEMETRY_SHARED_SECRET "$TELEMETRY_SECRETS_PATH")"
  env_token="$(env_value VIBE_STICK_BRIDGE_TOKEN "$ENV_PATH")"

  if is_placeholder_wifi "$telemetry_ssid"; then
    fail "Battery telemetry Wi-Fi SSID is empty or placeholder."
  else
    pass "Battery telemetry Wi-Fi SSID is configured."
  fi
  if is_placeholder_password "$telemetry_password"; then
    fail "Battery telemetry Wi-Fi password is empty or placeholder."
  else
    pass "Battery telemetry Wi-Fi password is configured."
  fi
  case "$telemetry_url" in
    http://127.0.0.1:*|http://0.0.0.0:*|http://192.168.1.10:*|"")
      fail "Battery telemetry bridge URL is loopback, wildcard, empty, or an example placeholder."
      ;;
    *)
      pass "Battery telemetry bridge URL is configured."
      ;;
  esac
  if [ "$telemetry_token" = "$env_token" ] && ! is_placeholder_token "$telemetry_token"; then
    pass "Battery telemetry token matches .env."
  else
    fail "Battery telemetry token is missing, placeholder, or differs from .env."
  fi
}

check_telemetry_health() {
  if command -v curl >/dev/null 2>&1 && curl -fsS http://127.0.0.1:8878/health >/dev/null 2>&1; then
    pass "Telemetry health endpoint responded on 127.0.0.1:8878."
  else
    warn "Telemetry health endpoint is not responding on 127.0.0.1:8878."
  fi
}

check_python
check_esp_idf
check_dotenv
check_secrets
check_token_match
check_telemetry_secrets
check_telemetry_health

printf 'SUMMARY pass=%s warn=%s fail=%s\n' "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"
