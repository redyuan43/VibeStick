#!/usr/bin/env python3
from __future__ import annotations

import json
import math
import struct
import sys
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TMP_DIR = ROOT / ".tmp"
LAST_REQUEST_PATH = TMP_DIR / "cyber-agent-last.json"
TTS_PATH = TMP_DIR / "cyber-agent-tts.wav"


def _write_test_tts(path: Path) -> None:
    sample_rate = 16000
    duration_s = 2.4
    frames = int(sample_rate * duration_s)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        for i in range(frames):
            t = i / sample_rate
            envelope = min(1.0, i / 1200) * min(1.0, (frames - i) / 1200)
            freq = 520 if int(t * 4) % 2 == 0 else 780
            tone = math.sin(2 * math.pi * freq * t) * envelope
            wav.writeframesraw(struct.pack("<h", int(tone * 14000)))


def main() -> int:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    payload = json.loads(sys.stdin.read() or "{}")
    LAST_REQUEST_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    _write_test_tts(TTS_PATH)
    intent = payload.get("intent") or "cyber"
    text = payload.get("text") or payload.get("transcript") or "收到"
    print(
        json.dumps(
            {
                "text": f"{intent} 已收到：{text}",
                "tts_audio_file": str(TTS_PATH),
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
