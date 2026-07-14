from __future__ import annotations

import csv
import json
import re
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from http import HTTPStatus
from io import StringIO
from pathlib import Path
from statistics import median
from typing import Any
from uuid import uuid4

from vibe_stick.config.paths import TELEMETRY_DIR

SAMPLE_CADENCE_SECONDS = 5
FRESH_SECONDS = 15
UNPLUG_WINDOW_SECONDS = 600
SMOKE_DURATION_SECONDS = 600
SMOKE_MIN_COVERAGE = 0.80
SMOKE_MAX_GAP_SECONDS = 20
SMOKE_RISING_TOLERANCE_MV = 30
FULL_MIN_START_MV = 4050
FULL_SHUTDOWN_MV = 3400
FULL_SHUTDOWN_SILENCE_SECONDS = 120
CHARGE_COMPLETE_MV = 4100
CHARGE_COMPLETE_SAMPLES = 3
SESSION_TIMEOUT_SECONDS = 12 * 60 * 60

_DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,64}$")
_SESSION_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,80}$")
_SESSION_KINDS = {"smoke", "full", "charge"}
_ACTIVE_STATUSES = {"waiting_for_unplug", "running"}


class TelemetryError(ValueError):
    def __init__(self, status: HTTPStatus, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


@dataclass(frozen=True)
class _SessionPaths:
    root: Path
    metadata: Path
    samples: Path
    summary: Path


class TelemetryStore:
    def __init__(self, root: Path = TELEMETRY_DIR) -> None:
        self.root = root
        self.devices_path = root / "devices.json"
        self.sessions_root = root / "sessions"
        self.raw_root = root / "raw"
        self._lock = threading.RLock()

    def create_session(self, payload: dict[str, Any]) -> dict[str, Any]:
        request = _validate_session_create(payload)
        now = _now()
        session_id = f"{request['kind']}_{now.strftime('%Y%m%dT%H%M%SZ')}_{uuid4().hex[:10]}"

        with self._lock:
            devices = self._load_devices_locked()
            device = devices.get(request["device_id"])
            if not device:
                raise TelemetryError(HTTPStatus.CONFLICT, "device has not reported telemetry")
            age = now.timestamp() - float(device.get("last_seen_epoch", 0))
            if age > FRESH_SECONDS:
                raise TelemetryError(HTTPStatus.CONFLICT, "device telemetry is stale")
            resume_unplugged = request["kind"] == "full" and request["resume_unplugged"]
            if device.get("usb_powered") is not True and not resume_unplugged:
                raise TelemetryError(HTTPStatus.CONFLICT, "device must be USB powered before the test")
            if (
                request["kind"] == "full"
                and not request["allow_partial"]
                and (
                    not isinstance(device.get("voltage_mv"), int)
                    or device["voltage_mv"] < FULL_MIN_START_MV
                )
            ):
                raise TelemetryError(
                    HTTPStatus.CONFLICT,
                    f"full test requires at least {FULL_MIN_START_MV} mV",
                )
            if device.get("active_session_id"):
                raise TelemetryError(HTTPStatus.CONFLICT, "device already has an active session")

            resumed_samples: list[dict[str, Any]] = []
            resumed_at = ""
            if resume_unplugged:
                raw_samples = self.list_raw_samples(
                    request["device_id"],
                    str(device.get("boot_id") or ""),
                )["samples"]
                resumed_at = _unplugged_at(raw_samples)
                if not resumed_at:
                    raise TelemetryError(
                        HTTPStatus.CONFLICT,
                        "no confirmed unplug transition in current boot telemetry",
                    )
                resumed_samples = [
                    {**sample, "session_id": session_id}
                    for sample in _samples_since(raw_samples, resumed_at)
                ]

            is_charge = request["kind"] == "charge"
            is_running = is_charge or resume_unplugged
            metadata = {
                "id": session_id,
                "device_id": request["device_id"],
                "board": device.get("board", ""),
                "kind": request["kind"],
                "status": "running" if is_running else "waiting_for_unplug",
                "created_at": _format_time(now),
                "started_at": resumed_at if resume_unplugged else (_format_time(now) if is_charge else ""),
                "unplugged_at": resumed_at,
                "stopped_at": "",
                "stop_reason": "",
                "allow_partial": request["allow_partial"],
                "resume_unplugged": request["resume_unplugged"],
                "label": request["label"],
                "config": _session_config(request["kind"], request["allow_partial"]),
            }
            paths = self._paths(session_id)
            paths.root.mkdir(parents=True, exist_ok=False)
            _write_json(paths.metadata, metadata)
            paths.samples.touch()
            for sample in resumed_samples:
                self._append_sample(session_id, sample)
            summary = self._build_summary(metadata, resumed_samples, now=now)
            _write_json(paths.summary, summary)
            self._update_device_session_locked(request["device_id"], session_id)

        return {"session": {**metadata, "summary": summary}}

    def stop_session(self, session_id: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        session_id = _validate_session_id(session_id)
        reason = str((payload or {}).get("reason") or "manual").strip()
        if not reason:
            reason = "manual"
        if len(reason) > 80:
            raise TelemetryError(HTTPStatus.BAD_REQUEST, "reason must be 80 characters or fewer")

        with self._lock:
            metadata = self._load_metadata(session_id)
            samples = self._load_samples(session_id)
            now = _now()
            if metadata["status"] in _ACTIVE_STATUSES:
                metadata["status"] = "stopped"
                metadata["stopped_at"] = _format_time(now)
                metadata["stop_reason"] = reason
                _write_json(self._paths(session_id).metadata, metadata)
            summary = self._build_summary(metadata, samples, now=now)
            _write_json(self._paths(session_id).summary, summary)
            self._clear_active_session_locked(metadata["device_id"], session_id)
        return {"session": {**metadata, "summary": summary}}

    def submit_sample(self, payload: dict[str, Any]) -> dict[str, Any]:
        sample = _validate_sample(payload)
        with self._lock:
            devices = self._load_devices_locked()
            device = devices.get(sample["device_id"], {})
            session_id = sample.get("session_id") or device.get("active_session_id") or ""
            if session_id:
                session_id = _validate_session_id(str(session_id))
                metadata = self._load_metadata(session_id)
                if metadata["device_id"] != sample["device_id"]:
                    raise TelemetryError(HTTPStatus.BAD_REQUEST, "sample device_id does not match session")
                if metadata["status"] not in _ACTIVE_STATUSES:
                    raise TelemetryError(HTTPStatus.CONFLICT, "session is not active")
                sample["session_id"] = session_id
                self._append_sample(session_id, sample)
                samples = self._load_samples(session_id)
                metadata = self._advance_session(metadata, samples, now=_now())
                paths = self._paths(session_id)
                _write_json(paths.metadata, metadata)
                summary = self._build_summary(metadata, samples, now=_now())
                _write_json(paths.summary, summary)
                if metadata["status"] not in _ACTIVE_STATUSES:
                    self._clear_active_session_locked(sample["device_id"], session_id)
            else:
                summary = None

            active_session_id = ""
            if session_id and metadata["status"] in _ACTIVE_STATUSES:
                active_session_id = session_id
            self._append_raw_sample(sample)
            devices[sample["device_id"]] = _device_record(sample, active_session_id)
            self._save_devices_locked(devices)

        response: dict[str, Any] = {"sample": sample, "device": devices[sample["device_id"]]}
        if session_id:
            response["session"] = {**metadata, "summary": summary}
        return response

    def list_devices(self) -> dict[str, Any]:
        with self._lock:
            devices = self._load_devices_locked()
        return {"devices": sorted(devices.values(), key=lambda item: item["device_id"])}

    def list_sessions(self) -> dict[str, Any]:
        with self._lock:
            sessions = []
            for metadata_path in sorted(self.sessions_root.glob("*/metadata.json")):
                metadata = _read_json(metadata_path)
                summary = _read_json(metadata_path.parent / "summary.json")
                sessions.append({**metadata, "summary": summary})
        sessions.sort(key=lambda item: item.get("created_at", ""), reverse=True)
        return {"sessions": sessions}

    def get_session(self, session_id: str) -> dict[str, Any]:
        session_id = _validate_session_id(session_id)
        with self._lock:
            metadata = self._load_metadata(session_id)
            samples = self._load_samples(session_id)
            metadata = self._advance_session(metadata, samples, now=_now())
            paths = self._paths(session_id)
            _write_json(paths.metadata, metadata)
            summary = self._build_summary(metadata, samples, now=_now())
            _write_json(paths.summary, summary)
            if metadata["status"] not in _ACTIVE_STATUSES:
                self._clear_active_session_locked(metadata["device_id"], session_id)
        return {"session": {**metadata, "summary": summary}}

    def list_samples(self, session_id: str) -> dict[str, Any]:
        session_id = _validate_session_id(session_id)
        with self._lock:
            self._load_metadata(session_id)
            samples = self._load_samples(session_id)
        return {"samples": samples}

    def export_csv(self, session_id: str) -> str:
        samples = self.list_samples(session_id)["samples"]
        return _samples_csv(samples)

    def list_raw_samples(self, device_id: str, boot_id: str = "") -> dict[str, Any]:
        device_id = _validate_device_id(device_id)
        boot_id = _optional_archive_id(boot_id)
        with self._lock:
            directory = self.raw_root / _archive_segment(device_id)
            paths = [directory / f"{_archive_segment(boot_id)}.jsonl"] if boot_id else sorted(directory.glob("*.jsonl"))
            samples: list[dict[str, Any]] = []
            for path in paths:
                samples.extend(_read_jsonl(path))
        samples.sort(key=lambda item: item["recorded_at_epoch"])
        return {"device_id": device_id, "boot_id": boot_id, "samples": samples}

    def export_raw_csv(self, device_id: str, boot_id: str = "") -> str:
        return _samples_csv(self.list_raw_samples(device_id, boot_id)["samples"])

    def _append_raw_sample(self, sample: dict[str, Any]) -> None:
        boot_id = sample.get("boot_id") or "unknown"
        path = (
            self.raw_root
            / _archive_segment(sample["device_id"])
            / f"{_archive_segment(str(boot_id))}.jsonl"
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(sample, sort_keys=True, separators=(",", ":")) + "\n")

    def _paths(self, session_id: str) -> _SessionPaths:
        root = self.sessions_root / session_id
        return _SessionPaths(
            root=root,
            metadata=root / "metadata.json",
            samples=root / "samples.jsonl",
            summary=root / "summary.json",
        )

    def _load_metadata(self, session_id: str) -> dict[str, Any]:
        path = self._paths(session_id).metadata
        if not path.exists():
            raise TelemetryError(HTTPStatus.NOT_FOUND, "session not found")
        metadata = _read_json(path)
        if not isinstance(metadata, dict):
            raise TelemetryError(HTTPStatus.INTERNAL_SERVER_ERROR, "session metadata is invalid")
        return metadata

    def _load_samples(self, session_id: str) -> list[dict[str, Any]]:
        path = self._paths(session_id).samples
        samples: list[dict[str, Any]] = []
        if not path.exists():
            return samples
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            data = json.loads(line)
            if isinstance(data, dict):
                samples.append(data)
        samples.sort(key=lambda item: item["recorded_at_epoch"])
        return samples

    def _append_sample(self, session_id: str, sample: dict[str, Any]) -> None:
        path = self._paths(session_id).samples
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(sample, sort_keys=True, separators=(",", ":")) + "\n")

    def _advance_session(
        self,
        metadata: dict[str, Any],
        samples: list[dict[str, Any]],
        *,
        now: datetime,
    ) -> dict[str, Any]:
        if metadata["status"] not in _ACTIVE_STATUSES:
            return metadata

        created_at = _parse_time(metadata["created_at"])
        elapsed = (now - created_at).total_seconds()
        if elapsed >= SESSION_TIMEOUT_SECONDS:
            return _finish(metadata, "failed", "timeout", now)

        if metadata["status"] == "waiting_for_unplug":
            unplugged_at = _unplugged_at(samples)
            if unplugged_at:
                metadata = dict(metadata)
                metadata["status"] = "running"
                metadata["started_at"] = unplugged_at
                metadata["unplugged_at"] = unplugged_at
                return metadata
            if elapsed > UNPLUG_WINDOW_SECONDS:
                return _finish(metadata, "failed", "unplug_timeout", now)
            return metadata

        analysis_samples = _samples_since(samples, metadata["started_at"])
        if metadata["kind"] == "charge":
            if _external_power_removed(analysis_samples):
                return _finish(metadata, "failed", "external_power_removed", now)
            if _charge_status(analysis_samples) == "passed":
                return _finish(metadata, "passed", "charge_complete", now)
            return metadata

        discharge_samples = analysis_samples
        if _external_power_returned(discharge_samples):
            return _finish(metadata, "failed", "external_power_returned", now)

        if metadata["kind"] == "smoke":
            smoke = _smoke_status(discharge_samples)
            if smoke == "passed":
                return _finish(metadata, "passed", "smoke_complete", now)
            if smoke:
                return _finish(metadata, "failed", smoke, now)
            return metadata

        full = _full_status(discharge_samples, now=now)
        if full == "passed":
            return _finish(metadata, "passed", "full_complete", now)
        if full:
            return _finish(metadata, "failed", full, now)
        return metadata

    def _build_summary(
        self,
        metadata: dict[str, Any],
        samples: list[dict[str, Any]],
        *,
        now: datetime,
    ) -> dict[str, Any]:
        analysis_samples = (
            _samples_since(samples, metadata["started_at"])
            if metadata.get("started_at")
            else []
        )
        valid_samples = [
            sample for sample in analysis_samples if isinstance(sample.get("voltage_mv"), int)
        ]
        first = valid_samples[0] if valid_samples else None
        last = valid_samples[-1] if valid_samples else None
        duration = 0.0
        max_gap = 0.0
        if len(analysis_samples) >= 2:
            duration = analysis_samples[-1]["recorded_at_epoch"] - analysis_samples[0]["recorded_at_epoch"]
            max_gap = max(
                analysis_samples[index]["recorded_at_epoch"]
                - analysis_samples[index - 1]["recorded_at_epoch"]
                for index in range(1, len(analysis_samples))
            )
        expected = int(duration / SAMPLE_CADENCE_SECONDS) + 1 if analysis_samples else 0
        coverage = min(1.0, len(analysis_samples) / expected) if expected else 0.0
        latest_received = analysis_samples[-1] if analysis_samples else None
        latest_age = (
            now.timestamp() - latest_received["recorded_at_epoch"]
            if latest_received
            else None
        )
        fresh = latest_age is not None and latest_age <= FRESH_SECONDS
        voltage_delta = (
            last["voltage_mv"] - first["voltage_mv"]
            if first is not None and last is not None
            else None
        )
        return {
            "session_id": metadata["id"],
            "status": metadata["status"],
            "stop_reason": metadata.get("stop_reason", ""),
            "phase": metadata["status"],
            "sample_count": len(analysis_samples),
            "first_sample_at": first["recorded_at"] if first else "",
            "last_sample_at": last["recorded_at"] if last else "",
            "latest_fresh": fresh,
            "latest_age_seconds": round(latest_age, 3) if latest_age is not None else None,
            "duration_seconds": round(duration, 3),
            "expected_samples": expected,
            "coverage": round(coverage, 4),
            "max_gap_seconds": round(max_gap, 3),
            "start_voltage_mv": first["voltage_mv"] if first else None,
            "last_voltage_mv": last["voltage_mv"] if last else None,
            "voltage_delta_mv": voltage_delta,
            "last_battery_percent": last.get("battery_percent") if last else None,
            "cadence_seconds": SAMPLE_CADENCE_SECONDS,
        }

    def _load_devices_locked(self) -> dict[str, dict[str, Any]]:
        if not self.devices_path.exists():
            return {}
        data = _read_json(self.devices_path)
        if not isinstance(data, dict):
            return {}
        devices = data.get("devices", {})
        return devices if isinstance(devices, dict) else {}

    def _save_devices_locked(self, devices: dict[str, dict[str, Any]]) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        _write_json(self.devices_path, {"devices": devices})

    def _update_device_session_locked(self, device_id: str, session_id: str) -> None:
        devices = self._load_devices_locked()
        record = devices.get(device_id, {"device_id": device_id})
        record["active_session_id"] = session_id
        devices[device_id] = record
        self._save_devices_locked(devices)

    def _clear_active_session_locked(self, device_id: str, session_id: str) -> None:
        devices = self._load_devices_locked()
        record = devices.get(device_id)
        if not record or record.get("active_session_id") != session_id:
            return
        record["active_session_id"] = ""
        devices[device_id] = record
        self._save_devices_locked(devices)


def _validate_session_create(payload: dict[str, Any]) -> dict[str, Any]:
    allowed = {"device_id", "kind", "allow_partial", "resume_unplugged", "label"}
    _reject_unknown(payload, allowed)
    device_id = _validate_device_id(payload.get("device_id"))
    kind = payload.get("kind")
    if kind not in _SESSION_KINDS:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "kind must be smoke, full, or charge")
    allow_partial = payload.get("allow_partial", False)
    if not isinstance(allow_partial, bool):
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "allow_partial must be a boolean")
    resume_unplugged = payload.get("resume_unplugged", False)
    if not isinstance(resume_unplugged, bool):
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "resume_unplugged must be a boolean")
    if resume_unplugged and kind != "full":
        raise TelemetryError(
            HTTPStatus.BAD_REQUEST,
            "resume_unplugged is only supported for full sessions",
        )
    label = payload.get("label", "")
    if not isinstance(label, str) or len(label) > 120:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "label must be a string up to 120 characters")
    return {
        "device_id": device_id,
        "kind": kind,
        "allow_partial": allow_partial,
        "resume_unplugged": resume_unplugged,
        "label": label,
    }


def _validate_sample(payload: dict[str, Any]) -> dict[str, Any]:
    allowed = {
        "schema_version",
        "device_id",
        "boot_id",
        "board",
        "pmic",
        "recorded_at",
        "battery_percent",
        "battery_mv",
        "usb_mv",
        "usb_powered",
        "charging",
        "session_id",
        "firmware_name",
        "firmware_version",
        "transport",
        "sequence",
        "uptime_ms",
        "sample_interval_ms",
        "wifi_connected",
    }
    _reject_unknown(payload, allowed)
    schema_version = payload.get("schema_version", 1)
    if schema_version != 1:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "schema_version must be 1")
    recorded_at = _parse_payload_time(payload.get("recorded_at", _format_time(_now())))
    voltage = _optional_int_field(payload, "battery_mv", minimum=2500, maximum=4500)
    percent = _optional_int_field(payload, "battery_percent", minimum=0, maximum=100)
    if percent is None and voltage is not None:
        percent = _estimate_percent(voltage)
    usb_voltage = _optional_int_field(payload, "usb_mv", minimum=0, maximum=6000)
    usb_powered = _optional_bool_field(payload, "usb_powered")
    charging = _optional_bool_field(payload, "charging")
    sample: dict[str, Any] = {
        "schema_version": 1,
        "device_id": _validate_device_id(payload.get("device_id")),
        "boot_id": _optional_text(payload, "boot_id", 64),
        "board": _optional_text(payload, "board", 32),
        "pmic": _optional_text(payload, "pmic", 32),
        "recorded_at": _format_time(recorded_at),
        "recorded_at_epoch": recorded_at.timestamp(),
        "battery_percent": percent,
        "voltage_mv": voltage,
        "usb_mv": usb_voltage,
        "usb_powered": usb_powered,
        "charging": charging,
        "session_id": "",
        "firmware_name": _optional_text(payload, "firmware_name", 64),
        "firmware_version": _optional_text(payload, "firmware_version", 64),
        "transport": _optional_text(payload, "transport", 32),
        "sequence": None,
        "uptime_ms": _optional_int_field(payload, "uptime_ms", minimum=0, maximum=2**63 - 1),
        "sample_interval_ms": _optional_int_field(
            payload,
            "sample_interval_ms",
            minimum=1000,
            maximum=60000,
        ),
        "wifi_connected": _optional_bool_field(payload, "wifi_connected"),
    }
    if "session_id" in payload and payload["session_id"] not in {"", None}:
        sample["session_id"] = _validate_session_id(payload["session_id"])
    if "sequence" in payload and payload["sequence"] is not None:
        sample["sequence"] = _int_field(payload, "sequence", minimum=0, maximum=2**31 - 1)
    return sample


def _device_record(sample: dict[str, Any], session_id: str) -> dict[str, Any]:
    return {
        "device_id": sample["device_id"],
        "boot_id": sample["boot_id"],
        "board": sample["board"],
        "pmic": sample["pmic"],
        "last_seen_at": sample["recorded_at"],
        "last_seen_epoch": sample["recorded_at_epoch"],
        "battery_percent": sample["battery_percent"],
        "voltage_mv": sample["voltage_mv"],
        "usb_mv": sample["usb_mv"],
        "usb_powered": sample["usb_powered"],
        "charging": sample["charging"],
        "firmware_name": sample["firmware_name"],
        "firmware_version": sample["firmware_version"],
        "transport": sample["transport"],
        "active_session_id": session_id,
        "sequence": sample["sequence"],
        "uptime_ms": sample["uptime_ms"],
        "sample_interval_ms": sample["sample_interval_ms"],
    }


def _session_config(kind: str, allow_partial: bool) -> dict[str, Any]:
    config: dict[str, Any] = {
        "cadence_seconds": SAMPLE_CADENCE_SECONDS,
        "fresh_seconds": FRESH_SECONDS,
        "unplug_window_seconds": UNPLUG_WINDOW_SECONDS,
        "timeout_seconds": SESSION_TIMEOUT_SECONDS,
    }
    if kind == "smoke":
        config.update(
            {
                "duration_seconds": SMOKE_DURATION_SECONDS,
                "min_coverage": SMOKE_MIN_COVERAGE,
                "max_gap_seconds": SMOKE_MAX_GAP_SECONDS,
                "rising_tolerance_mv": SMOKE_RISING_TOLERANCE_MV,
            }
        )
    elif kind == "full":
        config.update(
            {
                "min_start_voltage_mv": FULL_MIN_START_MV,
                "allow_partial": allow_partial,
                "shutdown_voltage_mv": FULL_SHUTDOWN_MV,
                "shutdown_silence_seconds": FULL_SHUTDOWN_SILENCE_SECONDS,
            }
        )
    else:
        config.update(
            {
                "complete_voltage_mv": CHARGE_COMPLETE_MV,
                "complete_samples": CHARGE_COMPLETE_SAMPLES,
            }
        )
    return config


def _smoke_status(samples: list[dict[str, Any]]) -> str:
    if len(samples) < 2:
        return ""
    duration = samples[-1]["recorded_at_epoch"] - samples[0]["recorded_at_epoch"]
    max_gap = max(
        samples[index]["recorded_at_epoch"] - samples[index - 1]["recorded_at_epoch"]
        for index in range(1, len(samples))
    )
    if max_gap > SMOKE_MAX_GAP_SECONDS:
        return "max_gap_exceeded"
    if duration < SMOKE_DURATION_SECONDS:
        return ""
    expected = int(duration / SAMPLE_CADENCE_SECONDS) + 1
    if len(samples) / expected < SMOKE_MIN_COVERAGE:
        return "coverage_low"
    voltages = [sample["voltage_mv"] for sample in samples if isinstance(sample.get("voltage_mv"), int)]
    if len(voltages) < 3:
        return "insufficient_voltage_samples"
    third = max(1, len(voltages) // 3)
    if median(voltages[-third:]) > median(voltages[:third]) + SMOKE_RISING_TOLERANCE_MV:
        return "voltage_rising"
    return "passed"


def _full_status(samples: list[dict[str, Any]], *, now: datetime) -> str:
    if not samples:
        return ""
    voltage_samples = [
        sample for sample in samples if isinstance(sample.get("voltage_mv"), int)
    ]
    if not voltage_samples:
        return ""
    last = voltage_samples[-1]
    if (
        last["voltage_mv"] <= FULL_SHUTDOWN_MV
        and now.timestamp() - last["recorded_at_epoch"] >= FULL_SHUTDOWN_SILENCE_SECONDS
    ):
        return "passed"
    return ""


def _charge_status(samples: list[dict[str, Any]]) -> str:
    if len(samples) < CHARGE_COMPLETE_SAMPLES:
        return ""
    recent = samples[-CHARGE_COMPLETE_SAMPLES:]
    if all(
        sample.get("usb_powered") is True
        and sample.get("charging") is False
        and isinstance(sample.get("voltage_mv"), int)
        and sample["voltage_mv"] >= CHARGE_COMPLETE_MV
        for sample in recent
    ):
        return "passed"
    return ""


def _unplugged_at(samples: list[dict[str, Any]]) -> str:
    if len(samples) < 3:
        return ""
    for index in range(2, len(samples)):
        window_samples = samples[index - 2:index + 1]
        window = (
            window_samples[-1]["recorded_at_epoch"]
            - window_samples[0]["recorded_at_epoch"]
        )
        if window <= UNPLUG_WINDOW_SECONDS and all(
            sample.get("usb_powered") is False and sample.get("charging") is False
            for sample in window_samples
        ):
            return window_samples[-1]["recorded_at"]
    return ""


def _samples_since(samples: list[dict[str, Any]], started_at: str) -> list[dict[str, Any]]:
    if not started_at:
        return []
    threshold = _parse_time(started_at).timestamp()
    return [sample for sample in samples if sample["recorded_at_epoch"] >= threshold]


def _external_power_returned(samples: list[dict[str, Any]]) -> bool:
    if len(samples) < 2:
        return False
    return all(
        sample.get("usb_powered") is True or sample.get("charging") is True
        for sample in samples[-2:]
    )


def _external_power_removed(samples: list[dict[str, Any]]) -> bool:
    if len(samples) < 2:
        return False
    return all(sample.get("usb_powered") is False for sample in samples[-2:])


def _finish(metadata: dict[str, Any], status: str, reason: str, now: datetime) -> dict[str, Any]:
    metadata = dict(metadata)
    metadata["status"] = status
    metadata["stop_reason"] = reason
    metadata["stopped_at"] = _format_time(now)
    return metadata


def _reject_unknown(payload: dict[str, Any], allowed: set[str]) -> None:
    unknown = sorted(set(payload) - allowed)
    if unknown:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, f"unknown field: {unknown[0]}")


def _validate_device_id(raw: Any) -> str:
    if not isinstance(raw, str) or not _DEVICE_ID_RE.fullmatch(raw):
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "device_id is required and must be 1-64 safe characters")
    return raw


def _validate_session_id(raw: Any) -> str:
    if not isinstance(raw, str) or not _SESSION_ID_RE.fullmatch(raw):
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "session_id is invalid")
    return raw


def _optional_archive_id(raw: Any) -> str:
    if raw in {"", None}:
        return ""
    if not isinstance(raw, str) or len(raw) > 64:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "boot_id must be up to 64 characters")
    return raw


def _archive_segment(raw: str) -> str:
    segment = re.sub(r"[^A-Za-z0-9_.-]", "_", raw)
    return segment or "unknown"


def _int_field(payload: dict[str, Any], name: str, *, minimum: int, maximum: int) -> int:
    value = payload.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, f"{name} must be an integer between {minimum} and {maximum}")
    return value


def _optional_int_field(
    payload: dict[str, Any],
    name: str,
    *,
    minimum: int,
    maximum: int,
) -> int | None:
    value = payload.get(name)
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise TelemetryError(
            HTTPStatus.BAD_REQUEST,
            f"{name} must be null or an integer between {minimum} and {maximum}",
        )
    return value


def _optional_bool_field(payload: dict[str, Any], name: str) -> bool | None:
    value = payload.get(name)
    if value is None:
        return None
    if not isinstance(value, bool):
        raise TelemetryError(HTTPStatus.BAD_REQUEST, f"{name} must be null or a boolean")
    return value


def _estimate_percent(voltage_mv: int) -> int:
    if voltage_mv <= 3300:
        return 0
    if voltage_mv >= 4150:
        return 100
    return round((voltage_mv - 3300) * 100 / 850)


def _optional_text(payload: dict[str, Any], name: str, limit: int) -> str:
    value = payload.get(name, "")
    if value is None:
        return ""
    if not isinstance(value, str) or len(value) > limit:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, f"{name} must be a string up to {limit} characters")
    return value


def _parse_payload_time(raw: Any) -> datetime:
    if not isinstance(raw, str) or not raw:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "recorded_at must be an ISO-8601 timestamp")
    try:
        return _parse_time(raw)
    except ValueError as exc:
        raise TelemetryError(HTTPStatus.BAD_REQUEST, "recorded_at must be an ISO-8601 timestamp") from exc


def _parse_time(raw: str) -> datetime:
    normalized = raw.replace("Z", "+00:00")
    parsed = datetime.fromisoformat(normalized)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _format_time(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    samples: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        data = json.loads(line)
        if isinstance(data, dict):
            samples.append(data)
    return samples


def _samples_csv(samples: list[dict[str, Any]]) -> str:
    out = StringIO()
    fields = [
        "recorded_at",
        "device_id",
        "boot_id",
        "session_id",
        "battery_percent",
        "voltage_mv",
        "usb_mv",
        "usb_powered",
        "charging",
        "sequence",
        "uptime_ms",
        "firmware_name",
        "firmware_version",
        "transport",
    ]
    writer = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
    writer.writeheader()
    writer.writerows(samples)
    return out.getvalue()


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
