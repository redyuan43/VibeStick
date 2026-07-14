from __future__ import annotations

import http.client
import json
import os
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock

from vibe_stick.protocol.state import default_state
from vibe_stick.server import app
from vibe_stick.telemetry.store import TelemetryStore


class _Store:
    def __init__(self, telemetry: TelemetryStore) -> None:
        self.telemetry = telemetry

    def get_state(self):  # noqa: ANN201
        state = default_state()
        state.battery = 72
        return state


class TelemetryServerTests(unittest.TestCase):
    def test_health_advertises_feature_and_state_keeps_battery_null(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with _server(TelemetryStore(Path(tmp))) as base:
                health = _request_json(base, "GET", "/health")
                state = _request_json(base, "GET", "/state")

        self.assertIn("battery_telemetry_v1", health["features"])
        self.assertIsNone(state["battery"])

    def test_protected_telemetry_post_requires_token(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.dict(os.environ, {"VIBE_STICK_BRIDGE_TOKEN": "secret"}):
                with _server(TelemetryStore(Path(tmp))) as base:
                    status, body = _request(base, "POST", "/telemetry/v1/sessions", {"device_id": "stick-a", "kind": "smoke"})
                    _request_json(
                        base,
                        "POST",
                        "/telemetry/v1/samples",
                        _sample_payload(),
                        token="secret",
                        expected=201,
                    )
                    authed = _request_json(
                        base,
                        "POST",
                        "/telemetry/v1/sessions",
                        {"device_id": "stick-a", "kind": "smoke"},
                        token="secret",
                        expected=201,
                    )

        self.assertEqual(status, 401)
        self.assertEqual(json.loads(body)["error"], "Unauthorized")
        self.assertEqual(authed["session"]["device_id"], "stick-a")

    def test_read_only_telemetry_routes_and_csv_export(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            telemetry = TelemetryStore(Path(tmp))
            telemetry.submit_sample(_sample_payload())
            session = telemetry.create_session({"device_id": "stick-a", "kind": "smoke"})["session"]
            for sequence in range(2, 5):
                telemetry.submit_sample(
                    {
                        **_sample_payload(),
                        "sequence": sequence,
                        "usb_powered": False,
                        "charging": False,
                    }
                )

            with _server(telemetry) as base:
                devices = _request_json(base, "GET", "/telemetry/v1/devices")
                sessions = _request_json(base, "GET", "/telemetry/v1/sessions")
                detail = _request_json(base, "GET", f"/telemetry/v1/sessions/{session['id']}")
                samples = _request_json(base, "GET", f"/telemetry/v1/sessions/{session['id']}/samples")
                status, csv_text = _request(base, "GET", f"/telemetry/v1/sessions/{session['id']}/export.csv")
                raw = _request_json(base, "GET", "/telemetry/v1/devices/stick-a/raw")
                raw_status, raw_csv = _request(
                    base,
                    "GET",
                    "/telemetry/v1/devices/stick-a/raw/export.csv",
                )

        self.assertEqual(devices["devices"][0]["device_id"], "stick-a")
        self.assertEqual(sessions["sessions"][0]["id"], session["id"])
        self.assertGreaterEqual(detail["session"]["summary"]["sample_count"], 1)
        self.assertEqual(samples["samples"][0]["battery_percent"], 88)
        self.assertEqual(status, 200)
        self.assertIn("recorded_at,device_id,boot_id,session_id", csv_text)
        self.assertGreaterEqual(len(raw["samples"]), 4)
        self.assertEqual(raw_status, 200)
        self.assertIn("recorded_at,device_id,boot_id,session_id", raw_csv)

    def test_dashboard_redirects_and_serves_static_assets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with _server(TelemetryStore(Path(tmp))) as base:
                status, _ = _request(base, "GET", "/telemetry")
                page_status, page = _request(base, "GET", "/telemetry/")
                css_status, css = _request(base, "GET", "/telemetry/telemetry.css")

        self.assertEqual(status, 303)
        self.assertEqual(page_status, 200)
        self.assertIn("VibeStick Telemetry", page)
        self.assertEqual(css_status, 200)
        self.assertIn("--accent", css)


class _server:
    def __init__(self, telemetry: TelemetryStore) -> None:
        self.telemetry = telemetry
        self.server: ThreadingHTTPServer | None = None
        self.thread: threading.Thread | None = None
        self.base = ""

    def __enter__(self) -> str:
        handler = app.make_handler(_Store(self.telemetry))  # type: ignore[arg-type]
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        host, port = self.server.server_address
        self.base = f"{host}:{port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self.base

    def __exit__(self, *exc: object) -> None:
        assert self.server is not None
        self.server.shutdown()
        self.server.server_close()
        assert self.thread is not None
        self.thread.join(timeout=2)


def _request_json(
    base: str,
    method: str,
    path: str,
    payload: dict[str, object] | None = None,
    *,
    token: str = "",
    expected: int = 200,
) -> dict[str, object]:
    status, body = _request(base, method, path, payload, token=token)
    if status != expected:
        raise AssertionError(f"expected {expected}, got {status}: {body}")
    return json.loads(body)


def _request(
    base: str,
    method: str,
    path: str,
    payload: dict[str, object] | None = None,
    *,
    token: str = "",
) -> tuple[int, str]:
    conn = http.client.HTTPConnection(base, timeout=5)
    headers = {}
    body = None
    if payload is not None:
        body = json.dumps(payload)
        headers["Content-Type"] = "application/json"
    if token:
        headers["X-Vibe-Stick-Token"] = token
    conn.request(method, path, body=body, headers=headers)
    response = conn.getresponse()
    data = response.read().decode("utf-8")
    conn.close()
    return response.status, data


def _sample_payload() -> dict[str, object]:
    return {
        "schema_version": 1,
        "device_id": "stick-a",
        "boot_id": "boot-a",
        "board": "sticks3",
        "pmic": "m5pm1",
        "battery_mv": 4100,
        "battery_percent": 88,
        "usb_mv": 5000,
        "usb_powered": True,
        "charging": True,
        "sequence": 1,
        "uptime_ms": 5000,
        "sample_interval_ms": 5000,
        "wifi_connected": True,
    }


if __name__ == "__main__":
    unittest.main()
