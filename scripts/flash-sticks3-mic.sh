#!/usr/bin/env bash
# Build and flash the StickS3 microphone-mode firmware.
#
# The firmware keeps one long-lived upload session so the PC sees a live
# microphone instead of a per-dictation recording. Wi-Fi credentials come from
# firmware/sticks3/include/vibe_stick_secrets.h and are seeded into NVS on boot.
#
# Usage: bash scripts/flash-sticks3-mic.sh [/dev/ttyACM0]
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
ROOT_DIR="$(cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-sticks3-mic"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
XTENSA_BIN="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin"
ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/20241011/"
ESPTOOL_PY="$HOME/.workbuddy/binaries/python/envs/default/bin/python"

# NVS lives in its own region; clearing it removes stale Wi-Fi profiles without
# touching the OTA images or the animation partition.
NVS_OFFSET=0x9000
NVS_SIZE=0x6000
BAUD=115200

for path in "$IDF_PATH/tools/idf.py" "$XTENSA_BIN/xtensa-esp32s3-elf-gcc" "$ESPTOOL_PY"; do
  [ -e "$path" ] || { printf 'missing: %s\n' "$path" >&2; exit 1; }
done

if [ ! -e "$PORT" ]; then
  printf 'serial port not present: %s\n' "$PORT" >&2
  printf 'power the device on, then rerun this script\n' >&2
  exit 1
fi

cd "$ROOT_DIR"
export IDF_PATH IDF_PYTHON_ENV_PATH ESP_ROM_ELF_DIR
export PATH="$IDF_PYTHON_ENV_PATH/bin:$XTENSA_BIN:$PATH"
export IDF_COMPONENT_MANAGER=1

printf '== build ==\n'
python3 "$IDF_PATH/tools/idf.py" -C firmware/sticks3 -B "$BUILD_DIR" \
  -D VIBE_BOARD=sticks3 -D VIBE_STICKS3_MIC_MODE=ON \
  -D SDKCONFIG="$BUILD_DIR/sdkconfig" build

# Keep the link open across the erase: a hard reset here would drop the
# USB-Serial-JTAG node and the following write_flash would lose its port.
printf '== erase NVS ==\n'
sg dialout -c "$ESPTOOL_PY -m esptool --chip esp32s3 -p $PORT -b $BAUD \
  --before default_reset --after no_reset erase_region $NVS_OFFSET $NVS_SIZE"

printf '== flash ==\n'
sg dialout -c "$ESPTOOL_PY -m esptool --chip esp32s3 -p $PORT -b $BAUD \
  --before default_reset --after hard_reset write_flash \
  0x0 $BUILD_DIR/bootloader/bootloader.bin \
  0x8000 $BUILD_DIR/partition_table/partition-table.bin \
  0xf000 $BUILD_DIR/ota_data_initial.bin \
  0x20000 $BUILD_DIR/vibe_stick_sticks3.bin"

printf '== done ==\n'
printf 'watch boot log: sg dialout -c "cat %s"\n' "$PORT"
