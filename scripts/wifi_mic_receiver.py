#!/usr/bin/env python3
"""VibeStick Wi-Fi bridge: turn a StickS3 into a PC microphone.

The device streams 16 kHz mono audio over HTTP. This script accepts the
upload and either writes WAV files or feeds a FIFO that PipeWire exposes as a
normal microphone, so any application can select it as an input device.

Two modes:

    # dictation style: one WAV file per recording session
    python3 scripts/wifi_mic_receiver.py --output ~/VibeStick-Recordings

    # live microphone: audio goes into a FIFO consumed by PipeWire
    python3 scripts/wifi_mic_receiver.py --fifo /tmp/vibestick-mic.pipe

Set up the PipeWire side with scripts/vibestick-mic.sh.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time
import wave
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

MAX_AUDIO_BYTES = 2_000_000
SAMPLE_RATE = 16_000
SAMPLE_WIDTH = 2
CHANNELS = 1

IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
)
IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)

_lock = threading.Lock()
_sessions: dict[str, dict] = {}


def _log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def decode_ima_adpcm(data: bytes, block_bytes: int = 484, block_samples: int = 960) -> bytes:
    """Decode IMA ADPCM blocks produced by firmware/src/vibe_ima_adpcm.c.

    Each block carries a 4 byte header (int16 predictor, uint8 step index,
    uint8 reserved) followed by low-nibble-first sample codes. The encoder
    packs one boundary sample in the header, so a block of N samples stores
    N-1 nibbles and pads the final byte.
    """
    out = bytearray()
    total = len(data)
    offset = 0
    while offset + 4 <= total:
        end = min(offset + block_bytes, total)
        predictor = int.from_bytes(data[offset:offset + 2], "little", signed=True)
        step_index = min(data[offset + 2], 88)
        offset += 4
        predictor = max(-32768, min(32767, predictor))
        out += predictor.to_bytes(2, "little", signed=True)
        remaining = block_samples - 1
        while remaining > 0 and offset < end:
            packed = data[offset]
            offset += 1
            for code in (packed & 0x0F, packed >> 4):
                if remaining <= 0:
                    break
                step = IMA_STEP_TABLE[step_index]
                diff = step >> 3
                if code & 1:
                    diff += step >> 2
                if code & 2:
                    diff += step >> 1
                if code & 4:
                    diff += step
                predictor += -diff if code & 8 else diff
                predictor = max(-32768, min(32767, predictor))
                step_index += IMA_INDEX_TABLE[code & 7]
                step_index = max(0, min(88, step_index))
                out += predictor.to_bytes(2, "little", signed=True)
                remaining -= 1
    return bytes(out)


class FifoSink:
    """Writes PCM into a FIFO at real time so PipeWire never underruns."""

    def __init__(self, path: Path, chunk_ms: int = 20, cap_ms: int = 1000) -> None:
        self.path = path
        self.chunk_bytes = SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS * chunk_ms // 1000
        self.cap_bytes = SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS * cap_ms // 1000
        self._buffer = bytearray()
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def write(self, pcm: bytes) -> None:
        if not pcm:
            return
        with _lock:
            self._buffer += pcm
            extra = len(self._buffer) - self.cap_bytes
            if extra > 0:
                del self._buffer[:extra]

    def close(self) -> None:
        self._running = False

    def _run(self) -> None:
        fd = None
        silence = bytes(self.chunk_bytes)
        next_at = time.monotonic()
        while self._running:
            if fd is None:
                try:
                    fd = os.open(self.path, os.O_WRONLY)
                    _log(f"fifo writer attached to {self.path}")
                except OSError:
                    time.sleep(0.2)
                    next_at = time.monotonic()
                    continue
            with _lock:
                if len(self._buffer) >= self.chunk_bytes:
                    frame = bytes(self._buffer[:self.chunk_bytes])
                    del self._buffer[:self.chunk_bytes]
                else:
                    frame = silence
            try:
                os.write(fd, frame)
            except OSError:
                try:
                    os.close(fd)
                except OSError:
                    pass
                fd = None
                continue
            next_at += self.chunk_bytes / (SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)
            delay = next_at - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.monotonic()


class PacatSink:
    """Feeds PCM into a PipeWire null-sink through pacat.

    A FIFO blocks as soon as nothing is recording from it: PipeWire suspends an
    idle source and stops draining the pipe, so the writer stalls, the device's
    HTTP upload stalls behind it, and the firmware queue fills up with audio
    from minutes ago. A null-sink is always running, so pacat keeps consuming
    and the device never blocks. The buffer is capped so that any backlog is
    discarded instead of accumulating latency.
    """

    def __init__(self, device: str, chunk_ms: int = 20, cap_ms: int = 1500) -> None:
        self.device = device
        self.chunk_bytes = SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS * chunk_ms // 1000
        self.cap_bytes = SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS * cap_ms // 1000
        self._buffer = bytearray()
        self._cond = threading.Condition()
        self._running = True
        self._proc: subprocess.Popen | None = None
        self._written = 0
        self._dropped = 0
        self._started = time.monotonic()
        self._report_at = self._started
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def write(self, pcm: bytes) -> None:
        if not pcm:
            return
        with self._cond:
            self._buffer += pcm
            extra = len(self._buffer) - self.cap_bytes
            if extra > 0:
                del self._buffer[:extra]
                self._dropped += extra
            self._cond.notify()
        self._account(len(pcm))

    def _account(self, size: int) -> None:
        """Report the gap between audio time and wall time.

        Anything above a few hundred milliseconds means the device is pushing
        faster than real time, i.e. its capture queue is backlogged and the host
        is hearing old audio.
        """
        self._written += size
        now = time.monotonic()
        elapsed = now - self._report_at
        if elapsed < 5.0:
            return
        audio_s = self._written / (SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)
        lag = audio_s - (now - self._started)
        _log(
            f"audio {audio_s:.1f}s in {now - self._started:.1f}s "
            f"lag={lag:+.2f}s buffered={len(self._buffer) * 1000 // 32000}ms "
            f"dropped={self._dropped // 32}ms"
        )
        self._report_at = now

    def close(self) -> None:
        self._running = False
        with self._cond:
            self._cond.notify_all()

    def _spawn(self) -> subprocess.Popen:
        return subprocess.Popen(
            [
                "pacat",
                "--playback",
                f"--device={self.device}",
                "--format=s16le",
                f"--rate={SAMPLE_RATE}",
                f"--channels={CHANNELS}",
                "--latency-msec=40",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )

    def _run(self) -> None:
        while self._running:
            if self._proc is None or self._proc.poll() is not None:
                try:
                    self._proc = self._spawn()
                    _log(f"pacat attached to sink {self.device}")
                except OSError as exc:
                    _log(f"pacat spawn failed: {exc}")
                    time.sleep(0.5)
                    continue
            with self._cond:
                while self._running and len(self._buffer) < self.chunk_bytes:
                    self._cond.wait(0.5)
                if not self._running:
                    break
                frame = bytes(self._buffer[:self.chunk_bytes])
                del self._buffer[:self.chunk_bytes]
            try:
                self._proc.stdin.write(frame)
                self._proc.stdin.flush()
            except (BrokenPipeError, OSError, AttributeError):
                try:
                    if self._proc is not None:
                        self._proc.kill()
                except OSError:
                    pass
                self._proc = None


class Session:
    def __init__(self, session_id: str, encoding: str, fifo, path: Path | None) -> None:
        self.session_id = session_id
        self.encoding = encoding
        self.fifo = fifo
        self.path = path
        self.block_samples = 960
        self.chunks = 0
        self.bytes = 0
        self.started = time.time()
        self.handle = open(path, "wb") if path else None

    def feed(self, body: bytes) -> None:
        if self.encoding.startswith("ima-adpcm"):
            block_bytes = 4 + self.block_samples // 2
            body = decode_ima_adpcm(body, block_bytes, self.block_samples)
        if self.fifo is not None:
            self.fifo.write(body)
        elif self.handle is not None:
            self.handle.write(body)
        self.chunks += 1
        self.bytes += len(body)

    def finish(self) -> float:
        if self.handle is not None:
            self.handle.close()
        return self.bytes / (SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = "VibeStickMic/0.2"
    protocol_version = "HTTP/1.1"

    recordings_dir: Path = Path.home() / "VibeStick-Recordings"
    fifo: FifoSink | None = None
    token: str = ""

    def log_message(self, fmt: str, *args) -> None:
        return

    def _reply(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _ok(self, recording: dict | None = None) -> dict:
        payload = {"success": True, "ok": True}
        if recording is not None:
            payload["recording"] = recording
        return payload

    def _authorized(self) -> bool:
        if not self.token:
            return True
        return self.headers.get("X-Vibe-Stick-Token", "") == self.token

    def _guard(self) -> bool:
        if self._authorized():
            return True
        self._reply(401, {"error": "invalid token"})
        return False

    def _read_body(self, limit: int) -> bytes | None:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length <= 0:
            return b""
        if length > limit:
            return None
        return self.rfile.read(length)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            if not self._guard():
                return
            self._reply(200, {
                "ok": True,
                "bridge_id": os.environ.get("M5_VOICE_BRIDGE_ID", "vibestick-mic"),
                "bridge_label": "VibeStick microphone",
                "version": "0.2",
                "mode": "fifo" if self.fifo is not None else "file",
            })
            return
        if parsed.path == "/state":
            self._reply(200, {
                "time": time.strftime("%H:%M"),
                "wifi": True,
                "ble": False,
                "battery": None,
                "active_provider": "mic",
                "provider": {
                    "id": "mic",
                    "display_name": "PC microphone",
                    "implemented": True,
                    "status": "RUNNING",
                    "project": "vibestick",
                    "quota_5h_remaining": 100,
                    "quota_7d_remaining": 100,
                    "quota_updated_at": time.strftime("%H:%M"),
                    "quota_stale": False,
                },
                "alert": {"event_id": "", "type": "NONE", "message": ""},
                "bridge_name": "vibestick-mic",
                "bridge_version": "0.2",
            })
            return
        if parsed.path == "/ota/manifest":
            self._reply(404, {"error": "this bridge does not serve OTA images"})
            return
        self._reply(404, {"error": "not found"})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        if parsed.path == "/recording/start":
            if not self._guard():
                return
            raw = self._read_body(64_000) or b""
            try:
                info = json.loads(raw or b"{}")
            except json.JSONDecodeError:
                info = {}
            session_id = str(info.get("session_id") or f"local-{int(time.time())}")
            encoding = str(info.get("transport_encoding") or "pcm16")
            path = None
            if self.fifo is None:
                self.recordings_dir.mkdir(parents=True, exist_ok=True)
                safe = "".join(c for c in session_id if c.isalnum() or c in "-_") or "session"
                path = self.recordings_dir / f"{safe}.pcm"
            session = Session(session_id, encoding, self.fifo, path)
            with _lock:
                _sessions[session_id] = session
            _log(f"start session={session_id} encoding={encoding} event={info.get('event', '?')}")
            self._reply(200, self._ok({
                "session_id": session_id,
                "accepted_transport_encoding": encoding,
                "capture_mode": "device_upload",
                "expected_chunk_id": 0,
                "upload_retry_ms": 5000,
            }))
            return

        if parsed.path == "/recording/audio":
            if not self._guard():
                return
            session_id = (query.get("session_id") or [""])[0]
            chunk_raw = (query.get("chunk_id") or ["0"])[0]
            crc_raw = (query.get("chunk_crc32") or [""])[0]
            body = self._read_body(MAX_AUDIO_BYTES)
            if body is None:
                self._reply(413, {"error": "audio body too large"})
                return
            if not body:
                self._reply(400, {"error": "empty audio body"})
                return
            if crc_raw:
                actual = format(zlib.crc32(body) & 0xFFFFFFFF, "08x")
                if crc_raw.lower() != actual:
                    _log(f"crc mismatch session={session_id} chunk={chunk_raw}")
                    self._reply(400, {"error": "crc mismatch", "crc32": actual})
                    return
            with _lock:
                session = _sessions.get(session_id)
            if session is None:
                self._reply(409, {"error": "unknown or ended session"})
                return
            header = self.headers.get("X-Vibe-Stick-Audio-Encoding", "")
            if header and not session.encoding.startswith("ima-adpcm"):
                session.encoding = header
            block_samples = self.headers.get("X-Vibe-Stick-Audio-Block-Samples", "")
            if block_samples.isdigit() and int(block_samples) > 1:
                session.block_samples = int(block_samples)
            session.feed(body)
            self._reply(200, self._ok({
                "session_id": session_id,
                "expected_chunk_id": int(chunk_raw) + 1,
                "received_bytes": session.bytes,
            }))
            return

        if parsed.path == "/recording/stop":
            if not self._guard():
                return
            raw = self._read_body(64_000) or b""
            try:
                info = json.loads(raw or b"{}")
            except json.JSONDecodeError:
                info = {}
            session_id = str(info.get("session_id") or "")
            with _lock:
                session = _sessions.pop(session_id, None)
            if session is None:
                self._reply(409, {"error": "unknown session"})
                return
            seconds = session.finish()
            wav_path = ""
            if session.path is not None:
                data = session.path.read_bytes() if session.path.exists() else b""
                target = session.path.with_suffix(".wav")
                with wave.open(str(target), "wb") as handle:
                    handle.setnchannels(CHANNELS)
                    handle.setsampwidth(SAMPLE_WIDTH)
                    handle.setframerate(SAMPLE_RATE)
                    handle.writeframes(data)
                session.path.unlink(missing_ok=True)
                wav_path = str(target)
            _log(
                f"stop session={session_id} chunks={session.chunks} "
                f"seconds={seconds:.2f}" + (f" -> {wav_path}" if wav_path else "")
            )
            recording = {
                "session_id": session_id,
                "status": "pasted",
                "seconds": round(seconds, 2),
                "bytes": session.bytes,
                "chunks": session.chunks,
            }
            if wav_path:
                recording["wav"] = wav_path
            self._reply(200, self._ok(recording))
            return

        if parsed.path in ("/event", "/quota/refresh"):
            if not self._guard():
                return
            raw = self._read_body(64_000) or b""
            try:
                info = json.loads(raw or b"{}")
            except json.JSONDecodeError:
                info = {}
            if parsed.path == "/event" and info:
                _log(f"event {json.dumps(info, ensure_ascii=False)}")
            self._reply(200, self._ok())
            return

        self._reply(404, {"error": "not found"})


def main() -> int:
    parser = argparse.ArgumentParser(description="VibeStick microphone bridge")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--fifo", default="", help="feed this FIFO instead of writing WAV files")
    parser.add_argument(
        "--sink",
        default="",
        help="feed this PipeWire null-sink through pacat (preferred over --fifo)",
    )
    parser.add_argument(
        "--output",
        default=os.environ.get(
            "VIBE_STICK_RECORDINGS_DIR", str(Path.home() / "VibeStick-Recordings")
        ),
    )
    args = parser.parse_args()

    if args.sink:
        BridgeHandler.fifo = PacatSink(args.sink)
    elif args.fifo:
        fifo_path = Path(args.fifo)
        if not fifo_path.exists():
            os.mkfifo(fifo_path)
        BridgeHandler.fifo = FifoSink(fifo_path)
    else:
        recordings = Path(args.output).expanduser()
        recordings.mkdir(parents=True, exist_ok=True)
        BridgeHandler.recordings_dir = recordings

    BridgeHandler.token = os.environ.get("VIBE_STICK_BRIDGE_TOKEN", "")
    server = ThreadingHTTPServer((args.host, args.port), BridgeHandler)
    if args.sink:
        mode = f"sink {args.sink}"
    elif args.fifo:
        mode = f"fifo {args.fifo}"
    else:
        mode = f"wav {BridgeHandler.recordings_dir}"
    _log(f"listening on {args.host}:{args.port}, mode {mode}")
    if BridgeHandler.token:
        _log("shared token required")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        _log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
