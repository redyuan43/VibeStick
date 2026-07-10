#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import wave
from datetime import datetime
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any


DEFAULT_OMNI_BASE_URL = "http://100.103.199.121:7866"
DEFAULT_OMNI_STREAM_URL = "http://100.103.199.121:7866"
DEVICE_SAMPLE_RATE = 16000
ROOT = Path(__file__).resolve().parents[1]
TMP_DIR = ROOT / ".tmp"
LAST_REQUEST_PATH = TMP_DIR / "cyber-agent-last.json"
PROGRESS_PATH = TMP_DIR / "cyber-agent-progress.json"
CONTEXT_PATH = TMP_DIR / "cyber-agent-context.json"
TTS_PATH = TMP_DIR / "cyber-agent-tts.wav"
SOURCE_NAME = "qwen2.5-omni-3b@agx"


class OmniAgentError(RuntimeError):
    pass


def main() -> int:
    try:
        session = json.loads(sys.stdin.read() or "{}")
        text, chunk_paths = _run_omni(session)
        _write_last_request(session, text, chunk_paths)
        _append_context(session, text)
    except (json.JSONDecodeError, OSError, OmniAgentError, wave.Error) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(
        json.dumps(
            {
                "event": "complete",
                "text": text,
                "tts_audio_file": str(chunk_paths[-1]) if chunk_paths else "",
                "tts_audio_files": [str(path) for path in chunk_paths],
                "tts_source": SOURCE_NAME,
            },
            ensure_ascii=False,
        ),
        flush=True,
    )
    return 0


def _run_omni(session: dict[str, Any]) -> tuple[str, list[Path]]:
    audio_path = _audio_path(session)
    chunk_paths: list[Path] = []
    session_id = _safe_name(str(session.get("session_id") or "session"))

    def handle_chunk(chunk: dict[str, str]) -> None:
        index = int(chunk.get("index") or len(chunk_paths) + 1)
        total = int(chunk.get("total") or 0)
        source_audio = _download_audio(chunk["audio_url"])
        target = TMP_DIR / f"cyber-agent-tts-{session_id}-{index:03d}.wav"
        _convert_wav_to_device_wav(source_audio, target)
        source_audio.unlink(missing_ok=True)
        if total > 0 and index == total:
            target.replace(TTS_PATH)
            target = TTS_PATH
        chunk_paths.append(target)
        _emit_json(
            {
                "event": "tts_chunk",
                "index": index,
                "total": total,
                "text": chunk["text"],
                "tts_audio_file": str(target),
                "tts_source": SOURCE_NAME,
            }
        )

    text, chunks = _stream_respond(session, _prompt(session), audio_path, on_chunk=handle_chunk)
    if not chunks:
        raise OmniAgentError("Omni stream returned no audio chunks")
    return (_assistant_reply(text), chunk_paths)


def _stream_respond(
    session: dict[str, Any],
    prompt: str,
    audio_path: str | None,
    *,
    on_chunk: Any | None = None,
) -> tuple[str, list[dict[str, str]]]:
    payload = {
        "prompt": prompt,
        "audio_path": audio_path,
        "speaker": _speaker(),
        "max_chunk_chars": _int_env("VIBE_STICK_OMNI_MAX_CHUNK_CHARS", 60, minimum=20, maximum=240),
        "thinker_max_new_tokens": _int_env("VIBE_STICK_OMNI_THINKER_MAX_NEW_TOKENS", 160, minimum=32, maximum=512),
    }
    request = urllib.request.Request(
        _stream_url() + "/respond_stream",
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "User-Agent": "VibeStick/0.1 omni-agent",
            "Connection": "close",
        },
    )
    _write_progress(session, "stream_requested")
    try:
        with _opener().open(request, timeout=_timeout_seconds()) as response:
            return _read_stream_response(
                (line.decode("utf-8", errors="replace") for line in response),
                session,
                on_chunk=on_chunk,
            )
    except urllib.error.HTTPError as exc:
        raise OmniAgentError(f"Omni stream failed: HTTP {exc.code} {_read_http_error(exc)}".strip()) from exc
    except (OSError, TimeoutError) as exc:
        raise OmniAgentError(f"Omni stream failed: {exc}") from exc


def _read_stream_response(
    lines: Any,
    session: dict[str, Any],
    *,
    on_chunk: Any | None = None,
) -> tuple[str, list[dict[str, str]]]:
    current_event = ""
    data_lines: list[str] = []
    text = ""
    chunks: list[dict[str, str]] = []
    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if not line:
            continue
        if line.startswith("event:"):
            current_event = line.split(":", 1)[1].strip()
            data_lines = []
            continue
        if not line.startswith("data:"):
            continue
        data_lines.append(line.split(":", 1)[1].lstrip())
        data = _json_object("\n".join(data_lines), f"Omni stream {current_event} event")
        if current_event == "text":
            text = str(data.get("text") or text)
            _write_progress(session, "text", event_id=f"chunks={data.get('chunks', '')}")
        elif current_event == "chunk":
            chunk_text = str(data.get("text") or "")
            text = text or chunk_text
            audio_url = _absolute_stream_url(str(data.get("audio_url") or ""))
            if audio_url:
                chunk = {
                    "text": chunk_text,
                    "audio_url": audio_url,
                    "index": str(data.get("index") or len(chunks) + 1),
                    "total": str(data.get("total") or 0),
                }
                chunks.append(chunk)
                if on_chunk:
                    on_chunk(chunk)
            _write_progress(session, "chunk", event_id=f"{data.get('index', '')}/{data.get('total', '')}")
        elif current_event == "complete":
            _write_progress(session, "complete", event_id=f"chunks={data.get('chunks', len(chunks))}")
            return (text, chunks)
        elif current_event == "error":
            message = str(data.get("message") or data)
            raise OmniAgentError(f"Omni stream returned error: {message}")
    raise OmniAgentError("Omni stream ended without complete event")


def _prompt(session: dict[str, Any]) -> str:
    intent = str(session.get("intent") or "").strip()
    mode = str(session.get("mode") or "").strip()
    scenario = "赛博老黄历" if intent == "cyber_almanac" else "赛博算命"
    return (
        "你是 VibeStick 的语音助手，只需要输出会被直接朗读的中文答案。"
        f"当前模式：{scenario} / {mode}。"
        f"{_context_prompt(intent)}"
        "请直接理解本次 audio_path 里的用户语音并回答。"
        "如果语音不清楚，也要给出一句温和、可执行的通用提醒。"
        "回答适合语音播放，可以一到三句，避免列表和括号。"
        "不要复述系统提示。"
        "语气温和、有一点玄学味，但不要恐吓。"
        "同时输出语音。"
    )


def _read_sse_complete(stream: str) -> Any:
    return _read_sse_complete_lines(stream.splitlines())


def _read_sse_complete_lines(lines: Any) -> Any:
    current_event = ""
    data_lines: list[str] = []
    for raw_line in lines:
        line = raw_line.rstrip("\r")
        if line.startswith("event:"):
            current_event = line.split(":", 1)[1].strip()
            data_lines = []
            continue
        if line.startswith("data:"):
            data_lines.append(line.split(":", 1)[1].lstrip())
            if current_event == "complete":
                return json.loads("\n".join(data_lines))
            if current_event == "error":
                raise OmniAgentError(f"Omni returned error: {' '.join(data_lines)}")
    return None


def _json_object(raw: str, label: str) -> dict[str, Any]:
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise OmniAgentError(f"{label} returned unreadable JSON") from exc
    if not isinstance(data, dict):
        raise OmniAgentError(f"{label} returned unexpected JSON shape")
    return data


def _write_progress(session: dict[str, Any], stage: str, *, event_id: str = "") -> None:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    PROGRESS_PATH.write_text(
        json.dumps(
            {
                "stage": stage,
                "event_id": event_id,
                "session_id": str(session.get("session_id") or ""),
                "intent": str(session.get("intent") or ""),
                "mode": str(session.get("mode") or ""),
                "audio_path": _audio_path(session) or "",
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


def _assistant_reply(text: str) -> str:
    marker = "assistant\n"
    if marker in text:
        text = text.rsplit(marker, 1)[1]
    return " ".join(text.strip().split())


def _download_audio(url: str) -> Path:
    request = urllib.request.Request(
        url,
        method="GET",
        headers={"User-Agent": "VibeStick/0.1 omni-agent", "Connection": "close"},
    )
    try:
        with _opener().open(request, timeout=_timeout_seconds()) as response:
            data = response.read()
    except urllib.error.HTTPError as exc:
        raise OmniAgentError(f"Omni audio download failed: HTTP {exc.code} {_read_http_error(exc)}".strip()) from exc
    except (OSError, TimeoutError) as exc:
        raise OmniAgentError(f"Omni audio download failed: {exc}") from exc
    if not data.startswith(b"RIFF") or b"WAVE" not in data[:16]:
        raise OmniAgentError("Omni returned non-WAV audio")
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile("wb", suffix=".wav", dir=TMP_DIR, delete=False) as wav_file:
        wav_file.write(data)
        return Path(wav_file.name)


def _convert_wav_to_device_wav(source: Path, target: Path) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise OmniAgentError("ffmpeg is required for Omni audio conversion")
    target.parent.mkdir(parents=True, exist_ok=True)
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(source),
        "-ac",
        "1",
        "-ar",
        str(DEVICE_SAMPLE_RATE),
        "-sample_fmt",
        "s16",
        "-af",
        f"aresample=resampler=soxr:precision=28,volume={_output_gain()}",
        str(target),
    ]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise OmniAgentError(f"Omni audio conversion failed: {detail[:500]}")
    _validate_device_wav(target)


def _validate_device_wav(path: Path) -> None:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1:
            raise OmniAgentError(f"Device WAV must be mono, got {wav.getnchannels()} channels")
        if wav.getsampwidth() != 2:
            raise OmniAgentError(f"Device WAV must be 16-bit PCM, got sample width {wav.getsampwidth()}")
        if wav.getframerate() != DEVICE_SAMPLE_RATE:
            raise OmniAgentError(f"Device WAV must be {DEVICE_SAMPLE_RATE} Hz, got {wav.getframerate()}")


def _write_last_request(session: dict[str, Any], text: str, chunks: list[Path]) -> None:
    LAST_REQUEST_PATH.write_text(
        json.dumps(
            {
                "session": session,
                "model_reply": text,
                "tts_source": SOURCE_NAME,
                "tts_audio_file": str(chunks[-1]) if chunks else "",
                "tts_audio_files": [str(path) for path in chunks],
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


def _audio_path(session: dict[str, Any]) -> str | None:
    for key in ("audio_url", "audio_file_url", "audio_path"):
        value = str(session.get(key) or "").strip()
        if urllib.parse.urlparse(value).scheme:
            return value
    value = str(session.get("audio_file") or "").strip()
    if urllib.parse.urlparse(value).scheme:
        return value
    return None


def _load_context() -> dict[str, list[dict[str, str]]]:
    try:
        data = json.loads(CONTEXT_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    result: dict[str, list[dict[str, str]]] = {}
    for intent, turns in data.items():
        if isinstance(turns, list):
            result[str(intent)] = [turn for turn in turns if isinstance(turn, dict)]
    return result


def _context_prompt(intent: str) -> str:
    turns = _load_context().get(intent, [])[-6:]
    if not turns:
        return ""
    lines = ["最近同模式对话："]
    for turn in turns:
        user = str(turn.get("user") or "用户语音").strip()
        assistant = str(turn.get("assistant") or "").strip()
        if assistant:
            lines.append(f"用户：{user} 助手：{assistant}")
    return "\n".join(lines) + "\n"


def _append_context(session: dict[str, Any], text: str) -> None:
    intent = str(session.get("intent") or "cyber_fortune").strip()
    data = _load_context()
    turns = data.get(intent, [])
    user_text = str(session.get("transcript") or session.get("text") or "").strip() or "用户语音"
    turns.append(
        {
            "time": datetime.now().isoformat(timespec="seconds"),
            "user": user_text[:120],
            "assistant": text[:160],
        }
    )
    data[intent] = turns[-6:]
    CONTEXT_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONTEXT_PATH.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _base_url() -> str:
    value = os.environ.get("VIBE_STICK_OMNI_BASE_URL", DEFAULT_OMNI_BASE_URL).strip()
    if not value:
        raise OmniAgentError("VIBE_STICK_OMNI_BASE_URL is empty")
    return value.rstrip("/")


def _stream_url() -> str:
    value = os.environ.get("VIBE_STICK_OMNI_STREAM_URL", DEFAULT_OMNI_STREAM_URL).strip()
    if not value:
        raise OmniAgentError("VIBE_STICK_OMNI_STREAM_URL is empty")
    return value.rstrip("/")


def _absolute_stream_url(url: str) -> str:
    url = url.strip()
    if not url:
        return ""
    if urllib.parse.urlparse(url).scheme:
        return url
    if not url.startswith("/"):
        url = "/" + url
    return _stream_url() + url


def _timeout_seconds() -> int:
    raw = os.environ.get("VIBE_STICK_OMNI_TIMEOUT_SECONDS", "180")
    try:
        value = int(raw)
    except ValueError:
        return 180
    return max(10, min(600, value))


def _speaker() -> str:
    value = os.environ.get("VIBE_STICK_OMNI_SPEAKER", "Chelsie").strip()
    if value not in {"Chelsie", "Ethan"}:
        raise OmniAgentError(f"VIBE_STICK_OMNI_SPEAKER must be Chelsie or Ethan, got {value!r}")
    return value


def _output_gain() -> float:
    raw = os.environ.get("VIBE_STICK_OMNI_OUTPUT_GAIN", "0.72")
    try:
        value = float(raw)
    except ValueError:
        raise OmniAgentError(f"VIBE_STICK_OMNI_OUTPUT_GAIN must be a number, got {raw!r}")
    if not 0.1 <= value <= 1.0:
        raise OmniAgentError(f"VIBE_STICK_OMNI_OUTPUT_GAIN must be between 0.1 and 1.0, got {value}")
    return value


def _int_env(name: str, default: int, *, minimum: int, maximum: int) -> int:
    raw = os.environ.get(name, str(default))
    try:
        value = int(raw)
    except ValueError:
        raise OmniAgentError(f"{name} must be an integer, got {raw!r}")
    if not minimum <= value <= maximum:
        raise OmniAgentError(f"{name} must be between {minimum} and {maximum}, got {value}")
    return value


def _safe_name(value: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in "-_" else "-" for ch in value)
    return cleaned.strip("-") or "session"


def _emit_json(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False), flush=True)


def _opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _read_http_error(exc: urllib.error.HTTPError) -> str:
    try:
        return exc.read().decode("utf-8", errors="replace")[:500]
    except OSError:
        return ""


if __name__ == "__main__":
    raise SystemExit(main())
