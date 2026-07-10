#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from uuid import uuid4


DEFAULT_AGX_ASR_URL = "http://agx.taild500c8.ts.net:8001/api/asr/transcribe"


class TranscriptionError(RuntimeError):
    pass


def main() -> int:
    try:
        session = json.load(sys.stdin)
        audio_file = Path(str(session.get("audio_file") or "")).expanduser()
        text = transcribe(audio_file)
    except (json.JSONDecodeError, OSError, TranscriptionError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(text)
    return 0


def transcribe(audio_file: Path) -> str:
    if not audio_file.is_file():
        raise TranscriptionError(f"Audio file is not available: {audio_file}")

    endpoint = os.environ.get("VIBE_STICK_AGX_ASR_URL", DEFAULT_AGX_ASR_URL).strip()
    if not endpoint:
        raise TranscriptionError("VIBE_STICK_AGX_ASR_URL is empty")

    boundary = f"VibeStickAGXASR-{uuid4().hex}"
    request = urllib.request.Request(
        endpoint,
        data=_multipart_body(boundary, audio_file),
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "User-Agent": "VibeStick/0.1 agx-asr",
            "Connection": "close",
        },
    )
    timeout = _timeout_seconds()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        message = _read_http_error(exc)
        raise TranscriptionError(f"AGX ASR failed: HTTP {exc.code} {message}".strip()) from exc
    except (OSError, TimeoutError) as exc:
        raise TranscriptionError(f"AGX ASR failed: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise TranscriptionError("AGX ASR returned unreadable JSON") from exc

    text = _extract_text(payload)
    if not text:
        raise TranscriptionError("AGX ASR returned no transcript")
    return text


def _multipart_body(boundary: str, audio_file: Path) -> bytes:
    body = bytearray()

    def add_field(name: str, value: str) -> None:
        body.extend(f"--{boundary}\r\n".encode())
        body.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        body.extend(value.encode())
        body.extend(b"\r\n")

    add_field("use_vad", os.environ.get("VIBE_STICK_AGX_ASR_USE_VAD", "true"))
    add_field("use_punc", os.environ.get("VIBE_STICK_AGX_ASR_USE_PUNC", "true"))
    add_field("hotword", os.environ.get("VIBE_STICK_AGX_ASR_HOTWORD", ""))

    body.extend(f"--{boundary}\r\n".encode())
    body.extend(f'Content-Disposition: form-data; name="audio"; filename="{audio_file.name}"\r\n'.encode())
    body.extend(f"Content-Type: {_content_type(audio_file)}\r\n\r\n".encode())
    body.extend(audio_file.read_bytes())
    body.extend(b"\r\n")
    body.extend(f"--{boundary}--\r\n".encode())
    return bytes(body)


def _extract_text(payload: object) -> str:
    if not isinstance(payload, dict):
        return ""
    for key in ("text", "raw_asr_text", "optimized_text", "asr_text"):
        value = str(payload.get(key) or "").strip()
        if value:
            return value
    data = payload.get("data")
    if isinstance(data, dict):
        return _extract_text(data)
    return ""


def _timeout_seconds() -> int:
    raw = os.environ.get("VIBE_STICK_AGX_ASR_TIMEOUT_SECONDS", "30")
    try:
        value = int(raw)
    except ValueError:
        return 30
    return max(3, min(180, value))


def _content_type(audio_file: Path) -> str:
    suffix = audio_file.suffix.lower()
    if suffix == ".wav":
        return "audio/wav"
    if suffix == ".mp3":
        return "audio/mpeg"
    if suffix == ".ogg":
        return "audio/ogg"
    return "application/octet-stream"


def _read_http_error(exc: urllib.error.HTTPError) -> str:
    try:
        return exc.read().decode("utf-8", errors="replace")[:500]
    except OSError:
        return ""


if __name__ == "__main__":
    raise SystemExit(main())
