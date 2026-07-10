#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import wave
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SECRETS = ROOT / "firmware" / "sticks3" / "include" / "vibe_stick_secrets.h"
DEFAULT_AUDIO = ROOT / ".tmp" / "agx-real-tts.wav"


def _bridge_token() -> str:
    text = SECRETS.read_text(encoding="utf-8")
    match = re.search(r'^#define\s+VIBE_STICK_BRIDGE_TOKEN\s+"([^"]+)"', text, re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing VIBE_STICK_BRIDGE_TOKEN in {SECRETS}")
    return match.group(1)


def _request_json(
    base_url: str,
    path: str,
    payload: dict[str, object] | None = None,
    timeout: int = 10,
) -> dict[str, object]:
    data = None
    headers = {"X-Vibe-Stick-Token": _bridge_token()}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(f"{base_url}{path}", data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _request_bytes(base_url: str, path: str, timeout: int = 10) -> bytes:
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"X-Vibe-Stick-Token": _bridge_token()},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def _request_json_no_auth(base_url: str, path: str, timeout: int = 10) -> dict[str, object]:
    with urllib.request.urlopen(f"{base_url}{path}", timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _post_bytes(base_url: str, path: str, payload: bytes, timeout: int = 10) -> dict[str, object]:
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=payload,
        headers={
            "X-Vibe-Stick-Token": _bridge_token(),
            "Content-Type": "application/octet-stream",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _read_pcm(audio_file: Path) -> bytes:
    with wave.open(str(audio_file), "rb") as wav:
        if wav.getnchannels() == 1 and wav.getsampwidth() == 2 and wav.getframerate() == 16000:
            return wav.readframes(wav.getnframes())
    result = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(audio_file),
            "-f",
            "s16le",
            "-acodec",
            "pcm_s16le",
            "-ac",
            "1",
            "-ar",
            "16000",
            "pipe:1",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    return result.stdout


def _upload_pcm(base_url: str, session_id: str, pcm: bytes) -> None:
    offset = 0
    chunk_size = 64 * 1024
    while offset < len(pcm):
        chunk = pcm[offset : offset + chunk_size]
        result = _post_bytes(base_url, f"/recording/audio?session_id={session_id}", chunk)
        assert result["recording"]["session_id"] == session_id
        offset += len(chunk)


def _check_flow(base_url: str, session_id: str, intent: str, mode: str, pcm: bytes, ack: bool) -> None:
    start = _request_json(
        base_url,
        "/recording/start",
        {
            "event": "cyber_flow_check_start",
            "source": "check",
            "audio_source": "sticks3_pcm",
            "session_id": session_id,
            "intent": intent,
            "mode": mode,
        },
    )
    start_rec = start["recording"]
    assert start_rec["session_id"] == session_id
    assert start_rec["intent"] == intent
    _upload_pcm(base_url, session_id, pcm)

    stop = _request_json(
        base_url,
        "/recording/stop",
        {
            "event": "cyber_flow_check_stop",
            "source": "check",
            "session_id": session_id,
            "paste": False,
            "intent": intent,
            "mode": mode,
        },
        timeout=240,
    )
    stop_rec = stop["recording"]
    assert stop_rec["session_id"] == session_id
    assert stop_rec["status"] in {"cyber_processing", "cyber_done"}
    assert stop_rec["intent"] == intent
    assert stop_rec["transcript"] == ""
    tts_request_id = _wait_for_tts_request(base_url)
    wav = _request_bytes(base_url, "/recording/tts")
    assert wav.startswith(b"RIFF") and b"WAVE" in wav[:16]
    if ack:
        _request_json(
            base_url,
            "/event",
            {
                "event": "tts_probe_played",
                "session_id": session_id,
                "tts_playback_request_id": tts_request_id,
            },
        )
    else:
        _wait_for_tts_clear(base_url, tts_request_id)
    print(f"{mode}: {stop_rec['status']} tts={tts_request_id}")


def _wait_for_tts_request(base_url: str, timeout: int = 240) -> str:
    deadline = time.monotonic() + timeout
    last_status = ""
    while time.monotonic() < deadline:
        state = _request_json_no_auth(base_url, "/state")
        request_id = str(state.get("tts_playback_request_id") or "").strip()
        if request_id:
            return request_id
        recording = state.get("recording")
        if isinstance(recording, dict):
            last_status = str(recording.get("status") or "")
        time.sleep(1)
    raise RuntimeError(f"timed out waiting for TTS playback request, last recording status={last_status}")


def _wait_for_tts_clear(base_url: str, request_id: str, timeout: int = 90) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = _request_json_no_auth(base_url, "/state")
        current = str(state.get("tts_playback_request_id") or "").strip()
        if current != request_id:
            return
        time.sleep(1)
    raise RuntimeError(f"timed out waiting for real device to acknowledge TTS request {request_id}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check FORT/ALM bridge cyber flow.")
    parser.add_argument("--base-url", default="http://127.0.0.1:8765")
    parser.add_argument("--audio-file", type=Path, default=DEFAULT_AUDIO)
    parser.add_argument("--no-ack", action="store_true", help="Leave TTS playback acknowledgement to a real device.")
    args = parser.parse_args(argv)

    try:
        pcm = _read_pcm(args.audio_file)
        health = _request_json(args.base_url, "/health")
        assert health.get("bridge_name") in {"vibestick-bridge", "capswriter-m5-voice-bridge"}
        _check_flow(args.base_url, "check-fort-0001", "cyber_fortune", "FORT", pcm, not args.no_ack)
        _check_flow(args.base_url, "check-alm-0001", "cyber_almanac", "ALM", pcm, not args.no_ack)
    except (AssertionError, RuntimeError, urllib.error.URLError, TimeoutError) as exc:
        print(f"cyber bridge flow check failed: {exc}", file=sys.stderr)
        return 1
    print("cyber bridge flow check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
