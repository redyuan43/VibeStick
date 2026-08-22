#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT_DIR"

python3 -m compileall -q bridge/src scripts tests
python3 -m pytest -q
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}" \
  firmware/sticks3/tests/run-host-tests.sh
firmware/telemetry/tests/run-host-tests.sh
python3 scripts/check-firmware-architecture.py
for board in sticks3 stickc_plus stickc_plus_se cardputer_adv; do
  image="firmware/sticks3/build-$board/vibe_stick_$board.bin"
  if [[ -f "$image" ]]; then
    python3 scripts/check-firmware-size.py "$board"
  fi
done
bash -n scripts/setup.sh scripts/doctor.sh scripts/install.sh scripts/dev.sh \
  scripts/battery-firmware.sh scripts/battery-test.sh scripts/check.sh \
  scripts/check-firmware.sh scripts/firmware.sh scripts/release-firmware.sh
node --check bridge/src/vibe_stick/web/telemetry/telemetry.js
git diff --check

if [[ "${1:-}" == "--firmware" ]]; then
  scripts/check-firmware.sh
fi
