#!/usr/bin/env bash
set -euo pipefail

PORT="/dev/serial/by-id/usb-Hades2001_M5stack_455257E04F-if00-port0"

if [[ ! -e "$PORT" ]]; then
  echo "MiniJoy serial device not found: $PORT" >&2
  exit 1
fi

while fuser "$PORT" >/dev/null 2>&1; do
  echo "Waiting for serial port: $PORT"
  sleep 1
done

stty -F "$PORT" 115200 cs8 -cstopb -parenb -ixon -ixoff -crtscts \
  raw -echo -hupcl
echo "Reading MiniJoy logs. Press Ctrl+C to exit."
exec cat "$PORT"
