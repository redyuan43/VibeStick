#!/usr/bin/env bash
# Record a short sample from the VibeStick virtual microphone and report whether
# real speech reached it (SNR between the quiet and loud segments).
#
# Usage: bash scripts/mic-level-test.sh [seconds]
set -euo pipefail

SECONDS_TO_RECORD="${1:-10}"
OUT="/tmp/vibestick-level-$(date +%H%M%S).wav"
PY="$HOME/.workbuddy/binaries/python/envs/default/bin/python"

command -v parecord >/dev/null || { echo "parecord not found" >&2; exit 1; }

pactl list short sources | grep -q vibestick_mic || {
  echo "虚拟麦克风未加载，先运行 scripts/vibestick-mic.sh" >&2
  exit 1
}

echo "录音 ${SECONDS_TO_RECORD}s —— 现在对着设备正常说话（不要喊）"
parecord --device=vibestick_mic --format=s16le --rate=16000 --channels=1 \
         --file-format=wav "$OUT" &
REC_PID=$!
sleep "$SECONDS_TO_RECORD"
kill -INT "$REC_PID" 2>/dev/null || true
wait "$REC_PID" 2>/dev/null || true
echo "已保存 $OUT"

"$PY" /tmp/analyze_mic.py "$OUT" 2>/dev/null || \
  "$PY" - "$OUT" <<'PYEOF'
import math, struct, sys, wave
path = sys.argv[1]
with wave.open(path, "rb") as w:
    rate, frames = w.getframerate(), w.readframes(w.getnframes())
data = struct.unpack("<%dh" % (len(frames) // 2), frames)
win = int(rate * 0.1)
seg = [math.sqrt(sum(float(x)*x for x in data[i:i+win])/win)
       for i in range(0, len(data)-win+1, win)]
seg.sort()
if not seg:
    print("空录音"); raise SystemExit(1)
quiet, loud = seg[len(seg)//10], seg[-1]
db = lambda v: 20*math.log10(v/32768.0) if v > 0 else -120.0
print("静音 RMS=%.0f (%.0f dBFS)   最响 RMS=%.0f (%.0f dBFS)"
      % (quiet, db(quiet), loud, db(loud)))
snr = 20*math.log10((loud+1)/(quiet+1))
print("动态范围 %.1f dB" % snr)
print("判定:", "有语音，链路可用" if snr >= 12 else
            ("信号偏弱，需继续加增益" if snr >= 6 else "基本无语音，检查麦克风开孔是否被遮挡"))
PYEOF
