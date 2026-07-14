from __future__ import annotations

import argparse
import html
import hmac
import ipaddress
import json
import mimetypes
import os
import shutil
import socket
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

try:
    from zeroconf import IPVersion, ServiceInfo, Zeroconf
except ImportError:  # pragma: no cover - optional at runtime for editable installs
    IPVersion = None
    ServiceInfo = None
    Zeroconf = None

from vibe_stick import __version__ as BRIDGE_VERSION
from vibe_stick.audio.recorder import RecordingController
from vibe_stick.claude.usage import fetch_usage as fetch_claude_usage
from vibe_stick.claude.usage import to_quota_snapshot as claude_usage_to_quota
from vibe_stick.codex.quota import QuotaSnapshot, load_quota, save_quota
from vibe_stick.config.paths import CLAUDE_QUOTA_PATH, QUOTA_PATH, RECORDING_PATH, STATE_PATH, ensure_app_support
from vibe_stick.desktop.hud import hide_hud
from vibe_stick.protocol.state import (
    AlertState,
    AlertType,
    VibeStickState,
    AgentStatus,
    CodexState,
    ProviderState,
    default_state,
    event_id,
    now_time_text,
    state_from_dict,
)
from vibe_stick.providers.base import ProviderObservation
from vibe_stick.providers.claude import observe_claude
from vibe_stick.providers.codex import observe_codex
from vibe_stick.telemetry import TelemetryError, TelemetryStore

MANUAL_STATUS_SECONDS = 60
BRIDGE_NAME = "vibestick-bridge"
MDNS_HOSTNAME = "vibestick.local."
MDNS_SERVICE_NAME = "VibeStick Bridge._vibestick._tcp.local."
MDNS_SERVICE_TYPE = "_vibestick._tcp.local."
DEFAULT_MAX_RECORDING_AUDIO_BYTES = 2_000_000
DEFAULT_CLAUDE_USAGE_INTERVAL_SECONDS = 300
MIN_CLAUDE_USAGE_INTERVAL_SECONDS = 30
PLACEHOLDER_BRIDGE_TOKENS = {
    "change-this-shared-token",
    "paste-generated-token-here",
    "changeme",
    "change-me",
}
OTA_BOARDS = {"sticks3", "stickc_plus"}


class BridgeStateStore:
    def __init__(self) -> None:
        ensure_app_support()
        self._lock = threading.RLock()
        self._project_root = _resolve_project_root()
        self._manual_status_until = 0.0
        self._state = self._load_state()
        self._last_active_provider = self._state.active_provider or "codex"
        self._claude_quota = load_quota(CLAUDE_QUOTA_PATH)
        if not _has_quota(self._claude_quota):
            self._claude_quota = _claude_quota_from_state(self._state)
        self._claude_usage_last_attempt = 0.0
        self._claude_usage_last_success = 0.0
        quota = load_quota(QUOTA_PATH)
        self._state.codex.quota_5h_remaining = quota.quota_5h_remaining
        self._state.codex.quota_7d_remaining = quota.quota_7d_remaining
        self._state.codex.quota_updated_at = quota.quota_updated_at
        self._state.codex.quota_stale = quota.quota_stale
        self.recording = RecordingController(RECORDING_PATH)
        self.telemetry = TelemetryStore()
        self._devices: dict[str, dict[str, Any]] = {}
        self._events: list[dict[str, Any]] = []
        self._tts_playback_request_id = ""
        hide_hud()

    def register_device_request(
        self,
        *,
        client_ip: str,
        path: str,
        headers: Any,
        authorized: bool,
    ) -> None:
        firmware = str(headers.get("X-Vibe-Stick-Firmware-Name") or "")
        device_id = str(headers.get("X-Vibe-Stick-Device-Id") or "").strip()
        if not firmware and not device_id:
            return
        now = time.time()
        key = device_id or client_ip
        with self._lock:
            previous = self._devices.get(key, {})
            self._devices[key] = {
                "device_id": key,
                "client_ip": client_ip,
                "path": path,
                "authorized": authorized,
                "last_seen": now,
                "last_seen_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now)),
                "firmware_name": firmware,
                "firmware_version": str(headers.get("X-Vibe-Stick-Firmware-Version") or ""),
                "transport": str(headers.get("X-Vibe-Stick-Firmware-Transport") or ""),
                "build_date": str(headers.get("X-Vibe-Stick-Firmware-Build-Date") or ""),
                "board": str(headers.get("X-Vibe-Stick-Board") or ""),
                "device_ip": str(headers.get("X-Vibe-Stick-Device-Ip") or client_ip),
                "wifi_ssid": str(headers.get("X-Vibe-Stick-Wifi-Ssid") or ""),
                "wifi_bssid": str(headers.get("X-Vibe-Stick-Wifi-Bssid") or ""),
                "wifi_rssi": _int_header(headers.get("X-Vibe-Stick-Wifi-Rssi"), previous.get("wifi_rssi", 0)),
                "wake_cause": str(headers.get("X-Vibe-Stick-Wake-Cause") or ""),
                "wake_cause_code": str(headers.get("X-Vibe-Stick-Wake-Cause-Code") or ""),
                "wake_ext1": str(headers.get("X-Vibe-Stick-Wake-Ext1") or ""),
                "reset_reason": str(headers.get("X-Vibe-Stick-Reset-Reason") or ""),
                "reset_reason_code": str(headers.get("X-Vibe-Stick-Reset-Reason-Code") or ""),
                "boot_count": str(headers.get("X-Vibe-Stick-Boot-Count") or ""),
                "pmic_wake": str(headers.get("X-Vibe-Stick-Pmic-Wake") or ""),
                "pmic_irq": str(headers.get("X-Vibe-Stick-Pmic-Irq") or ""),
                "pmic_timer": str(headers.get("X-Vibe-Stick-Pmic-Timer") or ""),
                "pmic_gpio_wake": str(headers.get("X-Vibe-Stick-Pmic-Gpio-Wake") or ""),
            }

    def devices(self) -> list[dict[str, Any]]:
        with self._lock:
            return sorted(
                (dict(device) for device in self._devices.values()),
                key=lambda device: float(device.get("last_seen") or 0),
                reverse=True,
            )

    def recent_events(self) -> list[dict[str, Any]]:
        with self._lock:
            return list(reversed(self._events[-25:]))

    def tts_playback_request_id(self) -> str:
        with self._lock:
            return self._tts_playback_request_id

    def request_tts_playback(self) -> dict[str, Any]:
        with self._lock:
            self._tts_playback_request_id = event_id("tts-probe")
            self._append_event_locked(
                "tts_probe_requested",
                session_id=self.recording.session.session_id,
                status=self.recording.session.status,
            )
            return {
                "requested": True,
                "tts_playback_request_id": self._tts_playback_request_id,
                "recording": self.recording.session.to_jsonable(),
            }

    def _append_event_locked(self, event: str, **fields: Any) -> None:
        now = time.time()
        item = {
            "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now)),
            "event": event,
        }
        for key, value in fields.items():
            if value not in {None, ""}:
                item[key] = value
        self._events.append(item)
        del self._events[:-80]

    def debug_snapshot(self, *, include_sensitive: bool) -> dict[str, Any]:
        with self._lock:
            recording = self.recording.session.to_jsonable()
            if not include_sensitive:
                recording["transcript"] = _redacted_text(recording.get("transcript"))
                recording["agent_text"] = _redacted_text(recording.get("agent_text"))
                recording["tts_audio_file"] = _redacted_path(recording.get("tts_audio_file"))
                recording["audio_file"] = _redacted_path(recording.get("audio_file"))
            return {
                "bridge": {
                    "name": BRIDGE_NAME,
                    "version": BRIDGE_VERSION,
                    "token_required": bool(_bridge_token()),
                    "asr_command_configured": bool(os.environ.get("VIBE_STICK_TRANSCRIBE_CMD", "").strip()),
                    "cyber_agent_configured": bool(os.environ.get("VIBE_STICK_CYBER_AGENT_CMD", "").strip()),
                    "agx_asr_url": os.environ.get("VIBE_STICK_AGX_ASR_URL", "").strip() or "",
                },
                "recording": recording,
                "devices": self.devices(),
                "events": self.recent_events(),
                "sensitive": include_sensitive,
            }

    def get_state(self) -> VibeStickState:
        with self._lock:
            self._refresh_providers_locked()
            self._state.time = now_time_text()
            self._save_state_locked()
            return self._state

    def update_from_event(self, event: dict[str, Any]) -> VibeStickState:
        event_name = str(event.get("event") or "")
        session_id = str(event.get("session_id") or "")
        followup_key = {
            "button_followup_enter": "enter",
            "button_followup_escape": "escape",
        }.get(event_name)
        followup_allowed = False
        with self._lock:
            self._append_event_locked(
                event_name or "event",
                source=str(event.get("source") or ""),
                session_id=session_id,
                status=str(event.get("status") or ""),
                message=str(event.get("message") or ""),
            )
            requested_status = event.get("codex_status")
            if not requested_status and _is_agent_status(event.get("status")):
                requested_status = event.get("status")
            if requested_status:
                self._set_codex_status(str(requested_status), str(event.get("message") or ""))
                self._manual_status_until = time.monotonic() + MANUAL_STATUS_SECONDS
            elif event_name == "button_double":
                self.refresh_quota_locked()
            elif event_name == "button_short":
                self._state.alert = AlertState(event_id="", type=AlertType.NONE, message="")
            elif followup_key:
                followup_allowed = bool(
                    session_id and session_id == self.recording.session.session_id
                )
            self._save_state_locked()

        if followup_key:
            if followup_allowed:
                result = self.recording.paste_injector.press_key(followup_key)
                event_result = "followup_key_sent" if result.success else "followup_key_failed"
                with self._lock:
                    self._append_event_locked(
                        event_result,
                        source=str(event.get("source") or ""),
                        session_id=session_id,
                        status=followup_key,
                        message=result.message,
                    )
                    self._save_state_locked()
            else:
                with self._lock:
                    self._append_event_locked(
                        "followup_key_ignored",
                        source=str(event.get("source") or ""),
                        session_id=session_id,
                        status=followup_key,
                        message="Recording session did not match",
                    )
                    self._save_state_locked()
        return self._state

    def refresh_quota(self) -> VibeStickState:
        with self._lock:
            self.refresh_quota_locked()
            self._save_state_locked()
            return self._state

    def refresh_quota_locked(self) -> None:
        if self._state.active_provider == "claude":
            self._refresh_claude_usage_locked(force=True)
            self._state.provider = _provider_state_from_observation(
                self._apply_claude_quota(observe_claude(self._project_root))
            )
            return

        codex_observation = observe_codex(self._project_root)
        self._apply_codex_quota(codex_observation, force_stale=True)
        self._state.codex = _codex_state_from_observation(codex_observation)
        if self._state.active_provider == "codex":
            self._state.provider = _provider_state_from_observation(codex_observation)

    def start_recording(self, request: dict[str, Any] | None = None) -> dict[str, Any]:
        session = self.recording.start(request)
        with self._lock:
            self._append_event_locked(
                "recording_start",
                session_id=session.session_id,
                intent=session.intent,
                mode=session.mode,
                source=session.audio_source,
                status=session.status,
            )
            self._state.alert = AlertState(
                event_id="",
                type=AlertType.NONE,
                message="",
            )
            self._save_state_locked()
        return {"recording": session.to_jsonable(), "state": self.get_state().to_jsonable()}

    def stop_recording(self, request: dict[str, Any] | None = None) -> dict[str, Any]:
        session = self.recording.stop(request)
        with self._lock:
            self._append_event_locked(
                "recording_stop",
                session_id=session.session_id,
                intent=session.intent,
                mode=session.mode,
                status=session.status,
                transcript_source=session.transcript_source,
                transcript_chars=len(session.transcript),
                tts_available=bool(session.tts_audio_file),
            )
        return {"recording": session.to_jsonable(), "state": self.get_state().to_jsonable()}

    def upload_recording_audio(
        self,
        pcm: bytes,
        *,
        session_id: str = "",
        sample_rate: int = 16000,
        channels: int = 1,
        bits_per_sample: int = 16,
        append: bool = False,
    ) -> dict[str, Any]:
        session = self.recording.attach_pcm(
            pcm,
            session_id=session_id,
            sample_rate=sample_rate,
            channels=channels,
            bits_per_sample=bits_per_sample,
            append=append,
        )
        with self._lock:
            self._append_event_locked(
                "recording_audio",
                session_id=session.session_id,
                status=session.status,
                source=session.audio_source,
                bytes=len(pcm),
            )
        return {"recording": session.to_jsonable(), "state": self.get_state().to_jsonable()}

    def tts_audio_path(self) -> Path | None:
        path = Path(self.recording.session.tts_audio_file)
        if not self.recording.session.tts_audio_file or not path.is_file():
            return None
        with self._lock:
            self._append_event_locked(
                "tts_download",
                session_id=self.recording.session.session_id,
                status=self.recording.session.status,
                bytes=path.stat().st_size if path.exists() else 0,
            )
        return path

    def _refresh_providers_locked(self) -> None:
        codex_observation = observe_codex(self._project_root)
        claude_observation = observe_claude(self._project_root)
        self._apply_codex_quota(codex_observation)

        if time.monotonic() < self._manual_status_until:
            _apply_manual_codex_state(codex_observation, self._state)

        active_provider = _select_active_provider(
            _configured_provider(),
            self._last_active_provider,
            codex_observation,
            claude_observation,
        )
        self._last_active_provider = active_provider
        self._state.active_provider = active_provider

        if active_provider == "claude":
            self._refresh_claude_usage_locked(force=False)
            active_observation = self._apply_claude_quota(claude_observation)
        else:
            active_observation = codex_observation

        self._state.codex = _codex_state_from_observation(codex_observation)
        self._state.provider = _provider_state_from_observation(active_observation)
        self._apply_alert_from_observation(
            _select_alert_observation(active_observation, codex_observation, claude_observation)
        )

    def _apply_alert_from_observation(self, observation: ProviderObservation) -> None:
        try:
            alert_type = AlertType(observation.alert_type)
        except ValueError:
            alert_type = AlertType.NONE
        if alert_type in {AlertType.DONE, AlertType.APPROVAL, AlertType.ERROR} and observation.alert_event_id:
            self._state.alert = AlertState(
                event_id=observation.alert_event_id,
                type=alert_type,
                message=observation.alert_message,
            )
        else:
            self._state.alert = AlertState(event_id="", type=AlertType.NONE, message="")

    def _apply_codex_quota(self, observation: ProviderObservation, *, force_stale: bool = False) -> None:
        if observation.quota_5h_remaining is not None or observation.quota_7d_remaining is not None:
            refreshed = QuotaSnapshot(
                quota_5h_remaining=observation.quota_5h_remaining,
                quota_7d_remaining=observation.quota_7d_remaining,
                quota_updated_at=observation.quota_updated_at,
                quota_stale=observation.quota_stale,
            )
            save_quota(QUOTA_PATH, refreshed)
        else:
            existing = QuotaSnapshot(
                quota_5h_remaining=self._state.codex.quota_5h_remaining,
                quota_7d_remaining=self._state.codex.quota_7d_remaining,
                quota_updated_at=self._state.codex.quota_updated_at,
                quota_stale=self._state.codex.quota_stale,
            )
            if existing.quota_5h_remaining is None and existing.quota_7d_remaining is None:
                refreshed = existing
            else:
                refreshed = _stale_quota(existing)
            if force_stale:
                save_quota(QUOTA_PATH, refreshed)

        observation.quota_5h_remaining = refreshed.quota_5h_remaining
        observation.quota_7d_remaining = refreshed.quota_7d_remaining
        observation.quota_updated_at = refreshed.quota_updated_at
        observation.quota_stale = refreshed.quota_stale

    def _refresh_claude_usage_locked(self, *, force: bool) -> None:
        now = time.monotonic()
        interval = _claude_usage_interval_seconds()
        if not force and now - self._claude_usage_last_attempt < interval:
            return
        self._claude_usage_last_attempt = now

        usage = fetch_claude_usage()
        if usage is None:
            if _has_quota(self._claude_quota):
                self._claude_quota = _stale_quota(self._claude_quota)
                save_quota(CLAUDE_QUOTA_PATH, self._claude_quota)
            else:
                self._claude_quota = QuotaSnapshot()
            return

        self._claude_quota = claude_usage_to_quota(usage)
        save_quota(CLAUDE_QUOTA_PATH, self._claude_quota)
        self._claude_usage_last_success = now

    def _apply_claude_quota(self, observation: ProviderObservation) -> ProviderObservation:
        quota = self._current_claude_quota()
        observation.quota_5h_remaining = quota.quota_5h_remaining
        observation.quota_7d_remaining = quota.quota_7d_remaining
        observation.quota_updated_at = quota.quota_updated_at
        observation.quota_stale = quota.quota_stale
        return observation

    def _current_claude_quota(self) -> QuotaSnapshot:
        if (
            self._claude_quota.quota_5h_remaining is None
            and self._claude_quota.quota_7d_remaining is None
        ):
            return self._claude_quota
        if self._claude_usage_last_success and time.monotonic() - self._claude_usage_last_success > 30 * 60:
            return _stale_quota(self._claude_quota)
        return self._claude_quota

    def _set_codex_status(self, raw_status: str, message: str) -> None:
        try:
            status = AgentStatus(raw_status.upper())
        except ValueError:
            status = AgentStatus.UNKNOWN
        self._state.codex.status = status
        if self._state.active_provider == "codex":
            self._state.provider.status = status
        if status == AgentStatus.DONE:
            self._state.alert = AlertState(event_id("done"), AlertType.DONE, message or "Codex task completed")
        elif status == AgentStatus.APPROVAL:
            self._state.alert = AlertState(
                event_id("approval"),
                AlertType.APPROVAL,
                message or "Codex is waiting for approval",
            )
        elif status == AgentStatus.ERROR:
            self._state.alert = AlertState(event_id("error"), AlertType.ERROR, message or "Codex needs attention")
        else:
            self._state.alert = AlertState(event_id="", type=AlertType.NONE, message="")

    def _load_state(self) -> VibeStickState:
        try:
            return state_from_dict(json.loads(STATE_PATH.read_text()))
        except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError):
            return default_state()

    def _save_state_locked(self) -> None:
        STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
        STATE_PATH.write_text(json.dumps(self._state.to_jsonable(), indent=2) + "\n")


def make_handler(store: BridgeStateStore) -> type[BaseHTTPRequestHandler]:
    class VibeStickHandler(BaseHTTPRequestHandler):
        server_version = "VibeStick/0.1"

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            authorized = self._is_authorized()
            store.register_device_request(
                client_ip=self.client_address[0],
                path=parsed.path,
                headers=self.headers,
                authorized=authorized,
            )
            if parsed.path in _protected_get_paths() and not authorized:
                self._send_error(HTTPStatus.UNAUTHORIZED, "Unauthorized")
                return

            if parsed.path == "/state":
                payload = _with_bridge_metadata(store.get_state().to_jsonable())
                request_id = store.tts_playback_request_id()
                if request_id:
                    payload["tts_playback_request_id"] = request_id
                self._send_json(payload)
            elif parsed.path == "/health":
                self._send_json(
                    {
                        "ok": True,
                        **_bridge_metadata(),
                        "features": ["battery_telemetry_v1"],
                    }
                )
            elif parsed.path == "/devices":
                self._send_json({"devices": store.devices()})
            elif parsed.path == "/debug/status":
                self._send_json(store.debug_snapshot(include_sensitive=self._can_view_sensitive_debug()))
            elif parsed.path in {"/", "/dashboard"}:
                self._send_html(
                    _dashboard_html(
                        store,
                        self.server.server_address,
                        include_sensitive=self._can_view_sensitive_debug(),
                    )
                )
            elif parsed.path == "/ota/manifest":
                board = _first(parse_qs(parsed.query), "board")
                self._send_json(_ota_manifest_payload(store._project_root, board))
            elif parsed.path == "/ota/bin":
                board = _first(parse_qs(parsed.query), "board")
                binary = _ota_binary_path(store._project_root, board)
                if binary is None:
                    self._send_error(HTTPStatus.NOT_FOUND, "OTA image not found")
                    return
                self._send_file(binary, "application/octet-stream")
            elif parsed.path == "/recording/tts":
                audio = store.tts_audio_path()
                if audio is None:
                    self._send_error(HTTPStatus.NOT_FOUND, "TTS audio not found")
                    return
                self._send_file(audio, _audio_content_type(audio))
            elif parsed.path.startswith("/telemetry"):
                self._handle_telemetry_get(parsed)
            else:
                self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")

        def do_POST(self) -> None:
            parsed = urlparse(self.path)
            authorized = self._is_authorized()
            store.register_device_request(
                client_ip=self.client_address[0],
                path=parsed.path,
                headers=self.headers,
                authorized=authorized,
            )
            if _is_protected_path(parsed.path) and not authorized:
                self._send_error(HTTPStatus.UNAUTHORIZED, "Unauthorized")
                return

            if parsed.path == "/event":
                body = self._read_json_body()
                self._send_json(store.update_from_event(body).to_jsonable())
            elif parsed.path == "/quota/refresh":
                state = store.refresh_quota()
                self._send_json({"refreshed": True, "state": state.to_jsonable()})
            elif parsed.path == "/debug/request-tts-playback":
                self._send_json(store.request_tts_playback())
            elif parsed.path == "/recording/start":
                body = self._read_json_body()
                result = store.start_recording(body)
                recording = result.get("recording", {})
                print(
                    "recording start "
                    f"session={recording.get('session_id', '')} "
                    f"intent={recording.get('intent', '')} "
                    f"mode={recording.get('mode', '')} "
                    f"source={recording.get('audio_source', '')}",
                    flush=True,
                )
                self._send_json(result)
            elif parsed.path == "/recording/audio":
                query = parse_qs(parsed.query)
                content_length = self._content_length()
                max_audio_bytes = _max_recording_audio_bytes()
                if content_length > max_audio_bytes:
                    self._send_error(
                        HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                        f"Recording audio exceeds {max_audio_bytes} bytes",
                    )
                    return
                pcm = self._read_raw_body(content_length)
                self._send_json(
                    store.upload_recording_audio(
                        pcm,
                        session_id=_first(query, "session_id"),
                        sample_rate=_int_header(self.headers.get("X-Vibe-Stick-Sample-Rate"), 16000),
                        channels=_int_header(self.headers.get("X-Vibe-Stick-Channels"), 1),
                        bits_per_sample=_int_header(self.headers.get("X-Vibe-Stick-Bits-Per-Sample"), 16),
                        append=_query_bool(query, "append"),
                    )
                )
            elif parsed.path == "/recording/stop":
                body = self._read_json_body()
                result = store.stop_recording(body)
                recording = result.get("recording", {})
                print(
                    "recording stop "
                    f"session={recording.get('session_id', '')} "
                    f"status={recording.get('status', '')} "
                    f"intent={recording.get('intent', '')} "
                    f"mode={recording.get('mode', '')} "
                    f"transcript_chars={len(str(recording.get('transcript', '')))} "
                    f"pasted={recording.get('pasted', False)}",
                    flush=True,
                )
                if _is_device_firmware_request(self.headers):
                    self._send_json(_device_recording_result(result))
                else:
                    self._send_json(result)
            elif parsed.path.startswith("/telemetry/v1/"):
                self._handle_telemetry_post(parsed)
            else:
                self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")

        def _handle_telemetry_get(self, parsed: Any) -> None:
            try:
                if parsed.path == "/telemetry/v1/devices":
                    self._send_json(store.telemetry.list_devices())
                elif _telemetry_device_raw_route(parsed.path, "export.csv"):
                    device_id = _telemetry_device_raw_route(parsed.path, "export.csv")
                    query = parse_qs(parsed.query)
                    self._send_text(
                        store.telemetry.export_raw_csv(device_id, _first(query, "boot_id")),
                        content_type="text/csv; charset=utf-8",
                    )
                elif _telemetry_device_raw_route(parsed.path, ""):
                    device_id = _telemetry_device_raw_route(parsed.path, "")
                    query = parse_qs(parsed.query)
                    self._send_json(
                        store.telemetry.list_raw_samples(device_id, _first(query, "boot_id"))
                    )
                elif parsed.path == "/telemetry/v1/sessions":
                    self._send_json(store.telemetry.list_sessions())
                elif _telemetry_session_route(parsed.path, "samples"):
                    session_id = _telemetry_session_route(parsed.path, "samples")
                    self._send_json(store.telemetry.list_samples(session_id))
                elif _telemetry_session_route(parsed.path, "export.csv"):
                    session_id = _telemetry_session_route(parsed.path, "export.csv")
                    self._send_text(
                        store.telemetry.export_csv(session_id),
                        content_type="text/csv; charset=utf-8",
                    )
                elif _telemetry_session_route(parsed.path, ""):
                    session_id = _telemetry_session_route(parsed.path, "")
                    self._send_json(store.telemetry.get_session(session_id))
                elif parsed.path == "/telemetry":
                    self._send_redirect("/telemetry/")
                elif parsed.path.startswith("/telemetry/"):
                    self._serve_telemetry_dashboard(parsed.path)
                else:
                    self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")
            except TelemetryError as exc:
                self._send_error(exc.status, exc.message)

        def _handle_telemetry_post(self, parsed: Any) -> None:
            try:
                if parsed.path == "/telemetry/v1/samples":
                    body = self._read_json_body()
                    body.setdefault(
                        "firmware_name",
                        self.headers.get("X-Vibe-Stick-Firmware-Name", ""),
                    )
                    body.setdefault(
                        "firmware_version",
                        self.headers.get("X-Vibe-Stick-Firmware-Version", ""),
                    )
                    body.setdefault(
                        "transport",
                        self.headers.get("X-Vibe-Stick-Firmware-Transport", "HTTP"),
                    )
                    self._send_json(store.telemetry.submit_sample(body), status=HTTPStatus.CREATED)
                elif parsed.path == "/telemetry/v1/sessions":
                    self._send_json(
                        store.telemetry.create_session(self._read_json_body()),
                        status=HTTPStatus.CREATED,
                    )
                elif _telemetry_session_route(parsed.path, "stop"):
                    session_id = _telemetry_session_route(parsed.path, "stop")
                    self._send_json(
                        store.telemetry.stop_session(session_id, self._read_json_body())
                    )
                else:
                    self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")
            except TelemetryError as exc:
                self._send_error(exc.status, exc.message)

        def _serve_telemetry_dashboard(self, request_path: str) -> None:
            dashboard_file = _telemetry_dashboard_file(request_path)
            if dashboard_file is None:
                self._send_error(HTTPStatus.NOT_FOUND, "Dashboard asset not found")
                return
            content_type = mimetypes.guess_type(str(dashboard_file))[0] or "application/octet-stream"
            self._send_file(dashboard_file, content_type)

        def log_message(self, fmt: str, *args: object) -> None:
            firmware_name = self.headers.get("X-Vibe-Stick-Firmware-Name", "-")
            firmware_version = self.headers.get("X-Vibe-Stick-Firmware-Version", "-")
            firmware_transport = self.headers.get("X-Vibe-Stick-Firmware-Transport", "-")
            print(
                f"{self.address_string()} - {fmt % args} "
                f"firmware={firmware_name}/{firmware_version} transport={firmware_transport}",
                flush=True,
            )

        def _read_json_body(self) -> dict[str, Any]:
            length = self._content_length()
            if length == 0:
                return {}
            raw = self.rfile.read(length)
            try:
                data = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                return {}
            return data if isinstance(data, dict) else {}

        def _read_raw_body(self, length: int) -> bytes:
            if length <= 0:
                return b""
            return self.rfile.read(length)

        def _content_length(self) -> int:
            try:
                length = int(self.headers.get("Content-Length", "0") or "0")
            except ValueError:
                return 0
            return max(0, length)

        def _is_authorized(self) -> bool:
            expected = _bridge_token()
            if not expected:
                return True
            supplied = self.headers.get("X-Vibe-Stick-Token", "")
            return hmac.compare_digest(supplied, expected)

        def _can_view_sensitive_debug(self) -> bool:
            return self._is_authorized() or _is_loopback_ip(self.client_address[0])

        def _send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "http://127.0.0.1")
            self.end_headers()
            self.wfile.write(data)

        def _send_html(self, body: str, status: HTTPStatus = HTTPStatus.OK) -> None:
            data = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "http://127.0.0.1")
            self.end_headers()
            self.wfile.write(data)

        def _send_text(
            self,
            payload: str,
            *,
            status: HTTPStatus = HTTPStatus.OK,
            content_type: str = "text/plain; charset=utf-8",
        ) -> None:
            data = payload.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "http://127.0.0.1")
            self.end_headers()
            self.wfile.write(data)

        def _send_redirect(self, location: str) -> None:
            self.send_response(HTTPStatus.SEE_OTHER)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _send_error(self, status: HTTPStatus, message: str) -> None:
            self._send_json({"error": message}, status=status)

        def _send_file(self, path: Path, content_type: str) -> None:
            try:
                size = path.stat().st_size
            except OSError:
                self._send_error(HTTPStatus.NOT_FOUND, "File not found")
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(size))
            self.send_header("Access-Control-Allow-Origin", "http://127.0.0.1")
            self.end_headers()
            with path.open("rb") as file:
                shutil.copyfileobj(file, self.wfile)

    return VibeStickHandler


def run_server(host: str, port: int) -> None:
    _enforce_bind_security(host)
    store = BridgeStateStore()
    server = ThreadingHTTPServer((host, port), make_handler(store))
    mdns = _start_mdns_advertisement(host, port)
    if not _bridge_token():
        print(
            "WARNING: VIBE_STICK_BRIDGE_TOKEN is not set; POST endpoints are unauthenticated on loopback only.",
            flush=True,
        )
    print(f"VibeStick Bridge listening on http://{host}:{port}", flush=True)
    try:
        server.serve_forever()
    finally:
        if mdns is not None:
            mdns.unregister_all_services()
            mdns.close()


def _protected_paths() -> set[str]:
    return {
        "/event",
        "/quota/refresh",
        "/debug/request-tts-playback",
        "/recording/start",
        "/recording/audio",
        "/recording/stop",
        "/telemetry/v1/samples",
        "/telemetry/v1/sessions",
    }


def _protected_get_paths() -> set[str]:
    return {
        "/health",
        "/ota/manifest",
        "/ota/bin",
        "/recording/tts",
    }


def _is_protected_path(path: str) -> bool:
    return path in _protected_paths() or bool(_telemetry_session_route(path, "stop"))


def _telemetry_session_route(path: str, leaf: str) -> str:
    prefix = "/telemetry/v1/sessions/"
    if not path.startswith(prefix):
        return ""
    remainder = path.removeprefix(prefix).strip("/")
    if not remainder:
        return ""
    parts = remainder.split("/")
    if leaf == "":
        return parts[0] if len(parts) == 1 else ""
    return parts[0] if len(parts) == 2 and parts[1] == leaf else ""


def _telemetry_device_raw_route(path: str, leaf: str) -> str:
    prefix = "/telemetry/v1/devices/"
    if not path.startswith(prefix):
        return ""
    remainder = path.removeprefix(prefix).strip("/")
    parts = remainder.split("/")
    expected = 2 if leaf == "" else 3
    if len(parts) != expected or parts[1] != "raw":
        return ""
    if leaf and parts[2] != leaf:
        return ""
    return unquote(parts[0])


def _telemetry_dashboard_file(request_path: str) -> Path | None:
    web_root = Path(__file__).resolve().parents[1] / "web" / "telemetry"
    if request_path == "/telemetry/":
        candidate = web_root / "index.html"
    else:
        candidate = web_root / request_path.removeprefix("/telemetry/").strip("/")
    try:
        resolved_root = web_root.resolve()
        resolved_candidate = candidate.resolve()
    except OSError:
        return None
    if resolved_root not in resolved_candidate.parents and resolved_candidate != resolved_root:
        return None
    return resolved_candidate if resolved_candidate.is_file() else None


def _audio_content_type(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".wav":
        return "audio/wav"
    if suffix == ".pcm":
        return "application/octet-stream"
    return "application/octet-stream"


def _start_mdns_advertisement(host: str, port: int) -> Any | None:
    if Zeroconf is None or ServiceInfo is None:
        print("WARNING: zeroconf is not installed; mDNS discovery is disabled.", flush=True)
        return None
    addresses = _advertised_addresses(host)
    if not addresses:
        print("WARNING: no LAN address available for mDNS discovery.", flush=True)
        return None
    info = ServiceInfo(
        MDNS_SERVICE_TYPE,
        MDNS_SERVICE_NAME,
        addresses=[socket.inet_aton(address) for address in addresses],
        port=port,
        properties={
            "bridge_name": BRIDGE_NAME,
            "bridge_version": BRIDGE_VERSION,
            "token_required": "1" if _bridge_token() else "0",
        },
        server=MDNS_HOSTNAME,
    )
    zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
    zeroconf.register_service(info)
    print(
        "VibeStick Bridge mDNS advertised as "
        f"{MDNS_HOSTNAME.rstrip('.')} -> {', '.join(addresses)}:{port}",
        flush=True,
    )
    return zeroconf


def _advertised_addresses(host: str) -> list[str]:
    normalized = host.strip()
    if normalized and normalized not in {"0.0.0.0", "::"}:
        try:
            address = ipaddress.ip_address(normalized.strip("[]"))
        except ValueError:
            return []
        return [str(address)] if isinstance(address, ipaddress.IPv4Address) and not address.is_loopback else []

    addresses: set[str] = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = info[4][0]
            ip = ipaddress.ip_address(address)
            if not ip.is_loopback:
                addresses.add(address)
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            address = sock.getsockname()[0]
            ip = ipaddress.ip_address(address)
            if not ip.is_loopback:
                addresses.add(address)
    except OSError:
        pass
    return sorted(addresses)


def _bridge_token() -> str:
    token = os.environ.get("VIBE_STICK_BRIDGE_TOKEN", "").strip()
    if token.lower() in PLACEHOLDER_BRIDGE_TOKENS:
        return ""
    return token


def _ota_dir(project_root: Path) -> Path:
    return project_root / "firmware" / "sticks3" / "ota"


def _safe_ota_board(raw: str) -> str:
    board = raw.strip()
    return board if board in OTA_BOARDS else ""


def _ota_manifest_path(project_root: Path, board: str) -> Path | None:
    board = _safe_ota_board(board)
    if not board:
        return None
    return _ota_dir(project_root) / f"{board}.json"


def _load_ota_manifest(project_root: Path, board: str) -> dict[str, Any] | None:
    manifest_path = _ota_manifest_path(project_root, board)
    if manifest_path is None:
        return None
    try:
        data = json.loads(manifest_path.read_text())
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    return data if isinstance(data, dict) else None


def _ota_manifest_payload(project_root: Path, board: str) -> dict[str, Any]:
    safe_board = _safe_ota_board(board)
    if not safe_board:
        return {"available": False, "error": "unknown board"}
    manifest = _load_ota_manifest(project_root, safe_board)
    if manifest is None:
        return {"available": False, "board": safe_board}
    payload = dict(manifest)
    payload["available"] = bool(payload.get("available", True))
    payload["board"] = safe_board
    payload.setdefault("url", f"/ota/bin?board={safe_board}")
    return payload


def _ota_binary_path(project_root: Path, board: str) -> Path | None:
    safe_board = _safe_ota_board(board)
    if not safe_board:
        return None
    manifest = _load_ota_manifest(project_root, safe_board)
    if manifest is None:
        return None
    file_name = str(manifest.get("file_name") or f"{safe_board}.bin")
    binary = _ota_dir(project_root) / Path(file_name).name
    try:
        binary.relative_to(_ota_dir(project_root))
    except ValueError:
        return None
    return binary if binary.exists() else None


def _enforce_bind_security(host: str) -> None:
    if _host_requires_token(host) and not _bridge_token():
        raise SystemExit(
            "Refusing to bind VibeStick Bridge outside loopback without "
            "VIBE_STICK_BRIDGE_TOKEN. Set a strong shared token or use --host 127.0.0.1."
        )


def _host_requires_token(host: str) -> bool:
    normalized = host.strip().strip("[]").lower()
    if normalized == "localhost":
        return False
    if not normalized:
        return True
    try:
        address = ipaddress.ip_address(normalized)
    except ValueError:
        return True
    return not address.is_loopback


def _max_recording_audio_bytes() -> int:
    raw = os.environ.get("VIBE_STICK_MAX_RECORDING_AUDIO_BYTES", "").strip()
    if not raw:
        return DEFAULT_MAX_RECORDING_AUDIO_BYTES
    try:
        value = int(raw)
    except ValueError:
        return DEFAULT_MAX_RECORDING_AUDIO_BYTES
    return max(256_000, min(8_000_000, value))


def _resolve_project_root() -> Path:
    configured = os.environ.get("VIBE_STICK_PROJECT_ROOT", "").strip()
    root = Path(configured).expanduser() if configured else Path.cwd()
    if root.name in {"bridge", "firmware", "app", "scripts"} and (root.parent / "README.md").exists():
        root = root.parent
    return root.resolve()


def _stale_quota(existing: QuotaSnapshot) -> QuotaSnapshot:
    return QuotaSnapshot(
        quota_5h_remaining=existing.quota_5h_remaining,
        quota_7d_remaining=existing.quota_7d_remaining,
        quota_updated_at=existing.quota_updated_at,
        quota_stale=True,
    )


def _has_quota(snapshot: QuotaSnapshot) -> bool:
    return snapshot.quota_5h_remaining is not None or snapshot.quota_7d_remaining is not None


def _claude_quota_from_state(state: VibeStickState) -> QuotaSnapshot:
    provider = state.provider
    if provider.id != "claude":
        return QuotaSnapshot()
    snapshot = QuotaSnapshot(
        quota_5h_remaining=provider.quota_5h_remaining,
        quota_7d_remaining=provider.quota_7d_remaining,
        quota_updated_at=provider.quota_updated_at,
        quota_stale=True,
    )
    return snapshot if _has_quota(snapshot) else QuotaSnapshot()


def _first(query: dict[str, list[str]], key: str) -> str:
    values = query.get(key) or []
    return values[0] if values else ""


def _query_bool(query: dict[str, list[str]], key: str) -> bool:
    return _first(query, key).strip().lower() in {"1", "true", "yes", "on"}


def _with_bridge_metadata(payload: dict[str, Any]) -> dict[str, Any]:
    payload.update(_bridge_metadata())
    return payload


def _bridge_metadata() -> dict[str, str]:
    bridge_id = (
        os.environ.get("VIBE_STICK_BRIDGE_ID", "").strip()
        or os.environ.get("M5_VOICE_BRIDGE_ID", "").strip()
        or BRIDGE_NAME
    )
    bridge_label = (
        os.environ.get("VIBE_STICK_BRIDGE_LABEL", "").strip()
        or os.environ.get("M5_VOICE_BRIDGE_LABEL", "").strip()
        or bridge_id
    )
    return {
        "bridge_name": BRIDGE_NAME,
        "bridge_version": BRIDGE_VERSION,
        "bridge_id": bridge_id,
        "bridge_label": bridge_label,
    }


def _is_device_firmware_request(headers: Any) -> bool:
    return bool(str(headers.get("X-Vibe-Stick-Firmware-Name") or "").strip())


def _device_recording_result(result: dict[str, Any]) -> dict[str, Any]:
    recording = result.get("recording")
    state = result.get("state")
    recording = recording if isinstance(recording, dict) else {}
    state = state if isinstance(state, dict) else {}
    alert = state.get("alert")
    alert = alert if isinstance(alert, dict) else {}
    return {
        "recording": {
            "session_id": str(recording.get("session_id") or ""),
            "status": str(recording.get("status") or ""),
            "message": str(recording.get("message") or ""),
            "intent": str(recording.get("intent") or ""),
            "mode": str(recording.get("mode") or ""),
            "transcript_source": str(recording.get("transcript_source") or "none"),
            "tts_available": bool(recording.get("tts_audio_file")),
        },
        "state": {
            "time": str(state.get("time") or ""),
            "wifi": bool(state.get("wifi", True)),
            "ble": bool(state.get("ble", False)),
            "active_provider": str(state.get("active_provider") or ""),
            "alert": {
                "event_id": str(alert.get("event_id") or ""),
                "type": str(alert.get("type") or "NONE"),
                "message": str(alert.get("message") or ""),
            },
        },
    }


def _redacted_text(value: object) -> str:
    return "[redacted]" if str(value or "").strip() else ""


def _redacted_path(value: object) -> str:
    raw = str(value or "").strip()
    if not raw:
        return ""
    return f".../{Path(raw).name}"


def _is_loopback_ip(raw: str) -> bool:
    try:
        return ipaddress.ip_address(raw).is_loopback
    except ValueError:
        return False


def _is_agent_status(value: object) -> bool:
    try:
        AgentStatus(str(value or "").upper())
    except ValueError:
        return False
    return True


def _dashboard_html(
    store: BridgeStateStore,
    server_address: tuple[Any, ...],
    *,
    include_sensitive: bool,
) -> str:
    host, port = server_address[:2]
    snapshot = store.debug_snapshot(include_sensitive=include_sensitive)
    devices = snapshot["devices"]
    events = snapshot.get("events", [])
    recording = snapshot["recording"]
    bridge = snapshot["bridge"]
    rows = "\n".join(_device_row(device) for device in devices)
    if not rows:
        rows = "<tr><td colspan=\"9\" class=\"empty\">No VibeStick devices seen yet.</td></tr>"
    event_rows = "\n".join(_event_row(event) for event in events)
    if not event_rows:
        event_rows = "<tr><td colspan=\"4\" class=\"empty\">No events yet.</td></tr>"
    recording_rows = "\n".join(
        _summary_row(label, value)
        for label, value in [
            ("Status", recording.get("status")),
            ("Intent", recording.get("intent")),
            ("Mode", recording.get("mode")),
            ("Audio Source", recording.get("audio_source")),
            ("Transcript Source", recording.get("transcript_source")),
            ("Message", recording.get("message")),
            ("Transcript", recording.get("transcript")),
            ("Model Reply", recording.get("agent_text")),
            ("TTS Source", recording.get("agent_source")),
            ("Audio File", recording.get("audio_file")),
            ("TTS File", recording.get("tts_audio_file")),
        ]
    )
    bridge_rows = "\n".join(
        _summary_row(label, value)
        for label, value in [
            ("Token Required", "yes" if bridge.get("token_required") else "no"),
            ("ASR Command", "configured" if bridge.get("asr_command_configured") else "not configured"),
            ("Cyber Agent", "configured" if bridge.get("cyber_agent_configured") else "not configured"),
            ("AGX ASR URL", bridge.get("agx_asr_url") or "default"),
            ("Detail View", "full" if snapshot.get("sensitive") else "redacted"),
        ]
    )
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VibeStick Bridge</title>
<style>
body {{ margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #111827; color: #f9fafb; }}
main {{ max-width: 1120px; margin: 0 auto; padding: 28px 20px; }}
h1 {{ margin: 0 0 8px; font-size: 28px; font-weight: 700; }}
.meta {{ color: #9ca3af; margin-bottom: 22px; }}
.grid {{ display: grid; grid-template-columns: minmax(0, 0.9fr) minmax(0, 1.1fr); gap: 16px; margin-bottom: 18px; }}
.panel {{ border: 1px solid #374151; background: #1f2937; }}
.panel h2 {{ margin: 0; padding: 12px 14px; border-bottom: 1px solid #374151; font-size: 16px; }}
table {{ width: 100%; border-collapse: collapse; background: #1f2937; border: 1px solid #374151; }}
.panel table {{ border: 0; }}
th, td {{ padding: 10px 12px; border-bottom: 1px solid #374151; text-align: left; font-size: 14px; }}
th {{ color: #d1d5db; background: #111827; font-weight: 600; }}
td:first-child {{ width: 150px; color: #9ca3af; }}
.value {{ max-width: 680px; overflow-wrap: anywhere; white-space: pre-wrap; }}
.empty {{ color: #9ca3af; text-align: center; padding: 24px; }}
.muted {{ color: #9ca3af; }}
.ok {{ color: #86efac; }}
.bad {{ color: #fca5a5; }}
@media (max-width: 820px) {{ .grid {{ grid-template-columns: 1fr; }} main {{ padding: 18px 12px; }} table {{ display: block; overflow-x: auto; }} }}
</style>
</head>
<body>
<main>
<h1>VibeStick Bridge</h1>
<div class="meta">Listening on {html.escape(str(host))}:{int(port)} &middot; mDNS: {MDNS_HOSTNAME.rstrip(".")}</div>
<section class="grid">
<div class="panel">
<h2>Bridge</h2>
<table><tbody>{bridge_rows}</tbody></table>
</div>
<div class="panel">
<h2>Latest Recording</h2>
<table><tbody>{recording_rows}</tbody></table>
</div>
</section>
<div class="panel">
<h2>Devices</h2>
<table>
<thead>
<tr><th>Device</th><th>IP</th><th>Board</th><th>Firmware</th><th>Wake</th><th>WiFi</th><th>RSSI</th><th>Last Seen</th><th>Last Path</th></tr>
</thead>
<tbody>
{rows}
</tbody>
</table>
</div>
<div class="panel events">
<h2>Recent Events</h2>
<table>
<thead>
<tr><th>Time</th><th>Event</th><th>Status</th><th>Details</th></tr>
</thead>
<tbody>
{event_rows}
</tbody>
</table>
</div>
<script>
setTimeout(() => window.location.reload(), 2000);
</script>
</main>
</body>
</html>
"""


def _summary_row(label: str, value: object) -> str:
    text = str(value or "")
    if not text:
        text = "-"
    return (
        "<tr>"
        f"<td>{html.escape(label)}</td>"
        f"<td class=\"value\">{html.escape(text)}</td>"
        "</tr>"
    )


def _device_row(device: dict[str, Any]) -> str:
    rssi = int(device.get("wifi_rssi") or 0)
    rssi_class = "ok" if rssi >= -67 else "bad" if rssi < -75 else "muted"
    firmware = " ".join(
        part for part in [
            str(device.get("firmware_name") or ""),
            str(device.get("firmware_version") or ""),
        ] if part
    )
    wake_parts = [
        str(device.get("reset_reason") or ""),
        str(device.get("wake_cause") or ""),
    ]
    wake_text = "/".join(part for part in wake_parts if part)
    if device.get("boot_count"):
        wake_text = f"{wake_text or '-'} #{device['boot_count']}"
    if device.get("pmic_wake"):
        wake_text += f" PMIC:{device['pmic_wake']}"
    if device.get("pmic_irq"):
        wake_text += f" IRQ:{device['pmic_irq']}"
    if device.get("pmic_timer"):
        wake_text += f" Timer:{device['pmic_timer']}"
    if device.get("pmic_gpio_wake"):
        wake_text += f" GPIO:{device['pmic_gpio_wake']}"
    return (
        "<tr>"
        f"<td>{html.escape(str(device.get('device_id') or ''))}</td>"
        f"<td>{html.escape(str(device.get('device_ip') or device.get('client_ip') or ''))}</td>"
        f"<td>{html.escape(str(device.get('board') or ''))}</td>"
        f"<td>{html.escape(firmware)}</td>"
        f"<td>{html.escape(wake_text or '-')}</td>"
        f"<td>{html.escape(str(device.get('wifi_ssid') or ''))}</td>"
        f"<td class=\"{rssi_class}\">{rssi}</td>"
        f"<td>{html.escape(str(device.get('last_seen_text') or ''))}</td>"
        f"<td class=\"muted\">{html.escape(str(device.get('path') or ''))}</td>"
        "</tr>"
    )


def _event_row(event: dict[str, Any]) -> str:
    status = str(event.get("status") or "")
    detail_parts = []
    for key in ("source", "session_id", "intent", "mode", "transcript_source", "transcript_chars", "tts_available", "bytes", "message"):
        value = event.get(key)
        if value not in {None, ""}:
            detail_parts.append(f"{key}={value}")
    return (
        "<tr>"
        f"<td>{html.escape(str(event.get('time') or ''))}</td>"
        f"<td>{html.escape(str(event.get('event') or ''))}</td>"
        f"<td>{html.escape(status)}</td>"
        f"<td class=\"value muted\">{html.escape(' '.join(detail_parts))}</td>"
        "</tr>"
    )


def _configured_provider() -> str:
    value = os.environ.get("VIBE_STICK_PROVIDER", "auto").strip().lower()
    return value if value in {"codex", "claude", "auto"} else "auto"


def _select_active_provider(
    configured: str,
    last_active: str,
    codex_observation: ProviderObservation,
    claude_observation: ProviderObservation,
) -> str:
    if configured in {"codex", "claude"}:
        return configured

    if codex_observation.online and not claude_observation.online:
        return "codex"
    if claude_observation.online and not codex_observation.online:
        return "claude"
    if codex_observation.online and claude_observation.online:
        codex_time = codex_observation.latest_event_timestamp
        claude_time = claude_observation.latest_event_timestamp
        if codex_time is not None and claude_time is not None:
            return "claude" if claude_time > codex_time else "codex"
        if claude_time is not None:
            return "claude"
        if codex_time is not None:
            return "codex"
        return last_active if last_active in {"codex", "claude"} else "codex"

    return last_active if last_active in {"codex", "claude"} else "codex"


def _select_alert_observation(
    active_observation: ProviderObservation,
    *observations: ProviderObservation,
) -> ProviderObservation:
    if _observation_has_alert(active_observation):
        return active_observation
    for observation in observations:
        if observation is active_observation:
            continue
        if _observation_has_alert(observation):
            return observation
    return active_observation


def _observation_has_alert(observation: ProviderObservation) -> bool:
    try:
        alert_type = AlertType(observation.alert_type)
    except ValueError:
        return False
    return alert_type in {AlertType.DONE, AlertType.APPROVAL, AlertType.ERROR} and bool(observation.alert_event_id)


def _claude_usage_interval_seconds() -> int:
    try:
        value = int(os.environ.get("VIBE_STICK_CLAUDE_USAGE_INTERVAL_SECONDS", ""))
    except ValueError:
        value = DEFAULT_CLAUDE_USAGE_INTERVAL_SECONDS
    if value <= 0:
        value = DEFAULT_CLAUDE_USAGE_INTERVAL_SECONDS
    return max(MIN_CLAUDE_USAGE_INTERVAL_SECONDS, value)


def _codex_state_from_observation(observation: ProviderObservation) -> CodexState:
    return CodexState(
        status=observation.status,
        project=observation.project,
        quota_5h_remaining=observation.quota_5h_remaining,
        quota_7d_remaining=observation.quota_7d_remaining,
        quota_updated_at=observation.quota_updated_at,
        quota_stale=observation.quota_stale,
    )


def _provider_state_from_observation(observation: ProviderObservation) -> ProviderState:
    return ProviderState(
        id=observation.provider_id,
        display_name=observation.display_name,
        implemented=True,
        status=observation.status,
        project=observation.project,
        quota_5h_remaining=observation.quota_5h_remaining,
        quota_7d_remaining=observation.quota_7d_remaining,
        quota_updated_at=observation.quota_updated_at,
        quota_stale=observation.quota_stale,
    )


def _apply_manual_codex_state(observation: ProviderObservation, state: VibeStickState) -> None:
    observation.status = state.codex.status
    observation.alert_type = state.alert.type.value
    observation.alert_message = state.alert.message
    observation.alert_event_id = state.alert.event_id


def _int_header(raw: str | None, default: int) -> int:
    try:
        value = int(raw or "")
    except ValueError:
        return default
    return value if value > 0 else default


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run VibeStick Bridge.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    run_server(args.host, args.port)
