#!/usr/bin/env bash
# Expose a VibeStick StickS3 as a normal PipeWire microphone.
#
# Creates a null-sink, republishes its monitor as the "VibeStick Mic" source,
# and runs the HTTP receiver that plays device audio into that sink through
# pacat. Any application can then pick "VibeStick Mic" as its input device.
#
# A null-sink is used instead of a FIFO on purpose: PipeWire suspends an idle
# source, and a suspended source stops draining the pipe. The blocked writer
# then stalls the device upload and the firmware queue fills with old audio, so
# the host hears speech from minutes ago. A sink is always running.
#
# Stop with Ctrl-C; the sink, the source and the receiver are removed.
set -euo pipefail

SINK_NAME="${VIBESTICK_SINK_NAME:-vibestick_sink}"
SOURCE_NAME="${VIBESTICK_SOURCE_NAME:-vibestick_mic}"
SOURCE_LABEL="${VIBESTICK_SOURCE_LABEL:-VibeStick Mic}"
PORT="${VIBESTICK_MIC_PORT:-8765}"
ROOT_DIR="$(cd -- "$(dirname -- "$0")/.." && pwd)"

if ! command -v pactl >/dev/null 2>&1; then
  printf '%s\n' "pactl not found. Install it with: sudo apt install pulseaudio-utils" >&2
  exit 127
fi
if ! command -v pacat >/dev/null 2>&1; then
  printf '%s\n' "pacat not found. Install it with: sudo apt install pulseaudio-utils" >&2
  exit 127
fi

cleanup() {
  if [ -n "${RECEIVER_PID:-}" ]; then
    kill "$RECEIVER_PID" 2>/dev/null || true
  fi
  if [ -n "${SOURCE_MODULE:-}" ]; then
    pactl unload-module "$SOURCE_MODULE" 2>/dev/null || true
  fi
  if [ -n "${SINK_MODULE:-}" ]; then
    pactl unload-module "$SINK_MODULE" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Drop leftovers from earlier runs so the names stay unique.
for module in $(pactl list short modules | awk -v s="$SOURCE_NAME" -v k="$SINK_NAME" \
  '($2 == "module-remap-source" && $0 ~ "source_name=" s) ||
   ($2 == "module-null-sink"    && $0 ~ "sink_name=" k) {print $1}'); do
  pactl unload-module "$module" 2>/dev/null || true
done

SINK_MODULE="$(pactl load-module module-null-sink \
  sink_name="$SINK_NAME" rate=16000 channels=1 \
  sink_properties="device.description=VibeStick_Sink")"
SOURCE_MODULE="$(pactl load-module module-remap-source \
  master="$SINK_NAME.monitor" source_name="$SOURCE_NAME" channels=1 \
  master_channel_map=mono channel_map=mono \
  source_properties="device.description=$SOURCE_LABEL")"

printf 'audio source ready: %s (sink %s, source %s)\n' \
  "$SOURCE_LABEL" "$SINK_MODULE" "$SOURCE_MODULE"

python3 "$ROOT_DIR/scripts/wifi_mic_receiver.py" \
  --host 0.0.0.0 --port "$PORT" --sink "$SINK_NAME" &
RECEIVER_PID=$!

printf 'receiver on 0.0.0.0:%s\n' "$PORT"
printf 'select "%s" as the input device in any application\n' "$SOURCE_LABEL"
wait "$RECEIVER_PID"
