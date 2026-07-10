#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
import warnings
import wave
from datetime import datetime
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any

warnings.filterwarnings("ignore", message="'audioop' is deprecated.*", category=DeprecationWarning)
import audioop


DEFAULT_AGX_TTS_URL = "http://agx.taild500c8.ts.net:18002/api/tts/speak"
DEFAULT_QWEN_BASE_URL = "http://agx.taild500c8.ts.net:11434/v1"
DEFAULT_QWEN_MODEL = "caps-voice-edit-qwen3-4b:latest"
DEVICE_SAMPLE_RATE = 16000
ROOT = Path(__file__).resolve().parents[1]
TMP_DIR = ROOT / ".tmp"
LAST_REQUEST_PATH = TMP_DIR / "cyber-agent-last.json"
TTS_PATH = TMP_DIR / "cyber-agent-tts.wav"
CYBER_SERVICES = {
    "cyber_fortune": {
        "name": "fortune",
        "scenario": "赛博算命",
        "system_role": "你是 VibeStick 的赛博算命助手。",
        "fallback_prefix": "提醒",
    },
    "cyber_almanac": {
        "name": "almanac",
        "scenario": "赛博老黄历",
        "system_role": "你是 VibeStick 的赛博老黄历助手。",
        "fallback_prefix": "老黄历",
    },
}


class CyberAgentError(RuntimeError):
    pass


def main() -> int:
    try:
        session = json.loads(sys.stdin.read() or "{}")
        service = _service_for_session(session)
        text = _generate_model_reply(session)
        source_wav = _synthesize_tts(text, session_id=str(session.get("session_id") or ""))
        _convert_wav_to_device_wav(source_wav, TTS_PATH)
        source_wav.unlink(missing_ok=True)
        _write_last_request(session, text, service)
    except (json.JSONDecodeError, OSError, CyberAgentError, wave.Error) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(
        json.dumps(
            {
                "text": text,
                "tts_audio_file": str(TTS_PATH),
                "tts_source": f"{_qwen_model()}+agx_tts",
                "service": service["name"],
            },
            ensure_ascii=False,
        )
    )
    return 0


def _service_for_session(session: dict[str, Any]) -> dict[str, str]:
    intent = str(session.get("intent") or "").strip()
    return CYBER_SERVICES.get(intent, CYBER_SERVICES["cyber_fortune"])


def _generate_model_reply(session: dict[str, Any]) -> str:
    endpoint = _qwen_endpoint()
    payload = _qwen_payload(session)
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "VibeStick/0.1 qwen-cyber-agent",
            "Connection": "close",
        },
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    data = _open_qwen(opener, request)

    text = _extract_qwen_text(data)
    if not text:
        raise CyberAgentError("Qwen returned no visible reply")
    return _compact_tts_text(text)


def _open_qwen(
    opener: urllib.request.OpenerDirector,
    request: urllib.request.Request,
) -> dict[str, Any]:
    try:
        with opener.open(request, timeout=_qwen_timeout_seconds()) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise CyberAgentError(f"Qwen failed: HTTP {exc.code} {_read_http_error(exc)}".strip()) from exc
    except (OSError, TimeoutError) as exc:
        raise CyberAgentError(f"Qwen failed: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CyberAgentError("Qwen returned unreadable JSON") from exc
    if not isinstance(data, dict):
        raise CyberAgentError("Qwen returned unreadable JSON")
    return data


def _qwen_payload(session: dict[str, Any]) -> dict[str, Any]:
    transcript = str(session.get("transcript") or "").strip()
    transcript_error = str(session.get("transcript_error") or "").strip()
    mode = str(session.get("mode") or "").strip()
    service = _service_for_session(session)
    today = datetime.now().strftime("%Y年%m月%d日")
    user_text = transcript or transcript_error or "给我一句今日提醒。"
    prompt = (
        f"日期：{today}\n"
        f"模式：{service['scenario']} / {mode}\n"
        f"用户语音：{user_text}\n"
        "请给出适合小屏幕和语音播放的一句话。"
    )
    return {
        "model": _qwen_model(),
        "messages": [
            {
                "role": "system",
                "content": (
                    service["system_role"] +
                    "不要展示推理过程。只输出最终答案。"
                    "用简体中文，口语、温和、有一点玄学味，但不要恐吓。"
                    "控制在 28 个汉字以内。"
                ),
            },
            {"role": "user", "content": prompt},
        ],
        "max_tokens": _int_env("VIBE_STICK_QWEN_MAX_TOKENS", 64, minimum=16, maximum=256),
        "temperature": _float_env("VIBE_STICK_QWEN_TEMPERATURE", 0.55),
        "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }


def _extract_qwen_text(data: object) -> str:
    if not isinstance(data, dict):
        return ""
    choices = data.get("choices")
    if not isinstance(choices, list) or not choices:
        return ""
    first = choices[0]
    if not isinstance(first, dict):
        return ""
    message = first.get("message")
    if isinstance(message, dict):
        content = str(message.get("content") or "").strip()
        if content:
            return content
    text = str(first.get("text") or "").strip()
    if text:
        return text
    return ""


def _compact_tts_text(text: str) -> str:
    cleaned = " ".join(text.replace("\n", " ").split())
    for prefix in ("最终答案：", "答案：", "答："):
        if cleaned.startswith(prefix):
            cleaned = cleaned[len(prefix):].strip()
    limit = _int_env("VIBE_STICK_QWEN_REPLY_MAX_CHARS", 42, minimum=12, maximum=80)
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[:limit].rstrip("，,。；; ") + "。"


def _answer_text(session: dict[str, Any]) -> str:
    transcript = str(session.get("transcript") or "").strip()
    service = _service_for_session(session)
    today = datetime.now().strftime("%m月%d日")
    if service["name"] == "almanac":
        return f"{today}老黄历：宜整理，忌冒进。{_short_hint(transcript)}"
    return f"{today}{service['fallback_prefix']}：先慢半拍。{_short_hint(transcript)}"


def _short_hint(transcript: str) -> str:
    if not transcript:
        return "今天稳一点就好。"
    normalized = transcript.replace("？", "").replace("?", "").strip()
    if any(word in normalized for word in ("出门", "出去", "出行")):
        return "出门看天气，钥匙证件确认。"
    if any(word in normalized for word in ("注意", "不宜", "忌")):
        return "少做临时决定，重要事留痕。"
    return "今天以稳为主。"


def _synthesize_tts(text: str, *, session_id: str) -> Path:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    endpoint = os.environ.get("VIBE_STICK_AGX_TTS_URL", DEFAULT_AGX_TTS_URL).strip()
    if not endpoint:
        raise CyberAgentError("VIBE_STICK_AGX_TTS_URL is empty")
    payload = {
        "text": text,
        "speaker": os.environ.get("VIBE_STICK_AGX_TTS_SPEAKER", "Vivian"),
        "speed": _float_env("VIBE_STICK_AGX_TTS_SPEED", 1.0),
        "language": os.environ.get("VIBE_STICK_AGX_TTS_LANGUAGE", "zh"),
        "trace_id": session_id,
    }
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "VibeStick/0.1 agx-tts",
            "Connection": "close",
        },
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=_timeout_seconds()) as response:
            audio = response.read()
    except urllib.error.HTTPError as exc:
        raise CyberAgentError(f"AGX TTS failed: HTTP {exc.code} {_read_http_error(exc)}".strip()) from exc
    except (OSError, TimeoutError) as exc:
        raise CyberAgentError(f"AGX TTS failed: {exc}") from exc
    if not audio.startswith(b"RIFF") or b"WAVE" not in audio[:16]:
        raise CyberAgentError("AGX TTS returned non-WAV audio")
    with NamedTemporaryFile("wb", suffix=".wav", dir=TMP_DIR, delete=False) as wav_file:
        wav_file.write(audio)
        return Path(wav_file.name)


def _convert_wav_to_device_wav(source: Path, target: Path) -> None:
    with wave.open(str(source), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.readframes(wav.getnframes())
    if sample_width != 2:
        raise CyberAgentError(f"AGX TTS WAV must be 16-bit PCM, got sample width {sample_width}")
    if channels == 2:
        frames = audioop.tomono(frames, sample_width, 0.5, 0.5)
    elif channels != 1:
        raise CyberAgentError(f"AGX TTS WAV must be mono or stereo, got {channels} channels")
    if sample_rate != DEVICE_SAMPLE_RATE:
        frames, _ = audioop.ratecv(frames, sample_width, 1, sample_rate, DEVICE_SAMPLE_RATE, None)

    target.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(target), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(DEVICE_SAMPLE_RATE)
        wav.writeframes(frames)


def _write_last_request(session: dict[str, Any], text: str, service: dict[str, str]) -> None:
    LAST_REQUEST_PATH.write_text(
        json.dumps(
            {
                "session": session,
                "service": service["name"],
                "model_reply": text,
                "tts_source": f"{_qwen_model()}+agx_tts",
                "tts_audio_file": str(TTS_PATH),
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


def _timeout_seconds() -> int:
    raw = os.environ.get("VIBE_STICK_AGX_TTS_TIMEOUT_SECONDS", "120")
    try:
        value = int(raw)
    except ValueError:
        return 120
    return max(10, min(300, value))


def _qwen_timeout_seconds() -> int:
    return _int_env("VIBE_STICK_QWEN_TIMEOUT_SECONDS", 90, minimum=10, maximum=300)


def _qwen_endpoint() -> str:
    base_url = os.environ.get("VIBE_STICK_QWEN_BASE_URL", DEFAULT_QWEN_BASE_URL).strip()
    if not base_url:
        raise CyberAgentError("VIBE_STICK_QWEN_BASE_URL is empty")
    return base_url.rstrip("/") + "/chat/completions"


def _qwen_model() -> str:
    model = os.environ.get("VIBE_STICK_QWEN_MODEL", DEFAULT_QWEN_MODEL).strip()
    if not model:
        raise CyberAgentError("VIBE_STICK_QWEN_MODEL is empty")
    return model


def _int_env(name: str, default: int, *, minimum: int, maximum: int) -> int:
    raw = os.environ.get(name, "")
    try:
        value = int(raw)
    except ValueError:
        return default
    return max(minimum, min(maximum, value))


def _float_env(name: str, default: float) -> float:
    raw = os.environ.get(name, "")
    try:
        return float(raw)
    except ValueError:
        return default


def _read_http_error(exc: urllib.error.HTTPError) -> str:
    try:
        return exc.read().decode("utf-8", errors="replace")[:500]
    except OSError:
        return ""


if __name__ == "__main__":
    raise SystemExit(main())
