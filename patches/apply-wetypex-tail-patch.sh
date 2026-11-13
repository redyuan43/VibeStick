#!/usr/bin/env bash
# Re-apply the WeTypeX tail-audio patch after a wetypex package upgrade.
#
# Symptom without it: the last words spoken before releasing the hold-to-talk
# key are cut off. stop_capture() SIGINTs pw-record the instant the key is
# released, so the ~1 s of audio still in flight through PipeWire (StickS3
# WiFi mic chain) never reaches the recorder.
#
# What the patch does (voice_stream.py):
#   1. stop_capture(): delay the pw-record SIGINT by 1.8 s (one-shot thread).
#   2. capture reader tail grace: 1 s -> 2.5 s after key release.
#
# Usage: sudo bash apply-wetypex-tail-patch.sh
set -euo pipefail

TARGET="/usr/share/fcitx5-wetypex/voice_stream.py"
PATCH="$(cd "$(dirname "$0")" && pwd)/wetypex-tail-audio.patch"

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$TARGET" ] || { echo "not found: $TARGET (wetypex not installed?)"; exit 1; }

if grep -q "_kill_scheduled" "$TARGET"; then
    echo "already patched, nothing to do"
    exit 0
fi

cp "$TARGET" "$TARGET.bak-tailfix"
patch -p0 "$TARGET" < "$PATCH"
python3 -m py_compile "$TARGET"
echo "patched: $TARGET (backup: $TARGET.bak-tailfix)"

# Restart the helper so it runs the patched code. fcitx5 does not respawn it
# automatically after a plain kill; start it again if it stayed down.
pkill -f "voice_stream.py serve" || true
sleep 2
if ! pgrep -f "voice_stream.py serve" >/dev/null; then
    sudo -u "${SUDO_USER:?}" nohup /usr/bin/fcitx5-wetypex-voice serve \
        >/dev/null 2>&1 &
fi
echo "voice_stream restarted"
