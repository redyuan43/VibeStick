from __future__ import annotations

import argparse
import hmac
import ipaddress
import json
import mimetypes
import os
import shutil
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse

from vibe_stick.telemetry.store import TelemetryError, TelemetryStore

DEFAULT_PORT = 8878
FEATURE_NAME = "battery_telemetry_v1"


def make_handler(store: TelemetryStore) -> type[BaseHTTPRequestHandler]:
    class TelemetryHandler(BaseHTTPRequestHandler):
        server_version = "VibeStickTelemetry/1"

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                self._send_json(
                    {
                        "ok": True,
                        "service_name": "vibestick-telemetry",
                        "features": [FEATURE_NAME],
                    }
                )
                return
            try:
                if parsed.path == "/telemetry/v1/devices":
                    self._send_json(store.list_devices())
                elif _device_raw_route(parsed.path, "export.csv"):
                    device_id = _device_raw_route(parsed.path, "export.csv")
                    self._send_text(
                        store.export_raw_csv(device_id, _first(parse_qs(parsed.query), "boot_id")),
                        content_type="text/csv; charset=utf-8",
                    )
                elif _device_raw_route(parsed.path, ""):
                    device_id = _device_raw_route(parsed.path, "")
                    self._send_json(
                        store.list_raw_samples(device_id, _first(parse_qs(parsed.query), "boot_id"))
                    )
                elif parsed.path == "/telemetry/v1/sessions":
                    self._send_json(store.list_sessions())
                elif _session_route(parsed.path, "samples"):
                    self._send_json(store.list_samples(_session_route(parsed.path, "samples")))
                elif _session_route(parsed.path, "export.csv"):
                    self._send_text(
                        store.export_csv(_session_route(parsed.path, "export.csv")),
                        content_type="text/csv; charset=utf-8",
                    )
                elif _session_route(parsed.path, ""):
                    self._send_json(store.get_session(_session_route(parsed.path, "")))
                elif parsed.path == "/telemetry":
                    self._send_redirect("/telemetry/")
                elif parsed.path.startswith("/telemetry/"):
                    self._serve_dashboard(parsed.path)
                else:
                    self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")
            except TelemetryError as exc:
                self._send_error(exc.status, exc.message)

        def do_POST(self) -> None:
            parsed = urlparse(self.path)
            if not _is_mutating_path(parsed.path):
                self._send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")
                return
            if not self._is_authorized():
                self._send_error(HTTPStatus.UNAUTHORIZED, "Unauthorized")
                return
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
                    self._send_json(store.submit_sample(body), status=HTTPStatus.CREATED)
                elif parsed.path == "/telemetry/v1/sessions":
                    self._send_json(
                        store.create_session(self._read_json_body()),
                        status=HTTPStatus.CREATED,
                    )
                else:
                    self._send_json(
                        store.stop_session(
                            _session_route(parsed.path, "stop"),
                            self._read_json_body(),
                        )
                    )
            except TelemetryError as exc:
                self._send_error(exc.status, exc.message)

        def log_message(self, fmt: str, *args: object) -> None:
            print(f"{self.address_string()} - {fmt % args}", flush=True)

        def _read_json_body(self) -> dict[str, Any]:
            try:
                length = max(0, int(self.headers.get("Content-Length", "0") or "0"))
            except ValueError:
                length = 0
            if length == 0:
                return {}
            try:
                value = json.loads(self.rfile.read(length).decode("utf-8"))
            except json.JSONDecodeError:
                return {}
            return value if isinstance(value, dict) else {}

        def _is_authorized(self) -> bool:
            expected = _telemetry_token()
            if not expected:
                return True
            return hmac.compare_digest(
                self.headers.get("X-Vibe-Stick-Token", ""),
                expected,
            )

        def _serve_dashboard(self, request_path: str) -> None:
            path = _dashboard_file(request_path)
            if path is None:
                self._send_error(HTTPStatus.NOT_FOUND, "Dashboard asset not found")
                return
            content_type = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
            self._send_file(path, content_type)

        def _send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
            self._send_bytes(
                json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                "application/json; charset=utf-8",
                status,
            )

        def _send_text(
            self,
            payload: str,
            *,
            content_type: str,
            status: HTTPStatus = HTTPStatus.OK,
        ) -> None:
            self._send_bytes(payload.encode("utf-8"), content_type, status)

        def _send_bytes(self, data: bytes, content_type: str, status: HTTPStatus) -> None:
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
            self._send_json({"error": message}, status)

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

    return TelemetryHandler


def run_server(host: str, port: int) -> None:
    _enforce_bind_security(host)
    server = ThreadingHTTPServer((host, port), make_handler(TelemetryStore()))
    print(f"VibeStick telemetry listening on http://{host}:{port}", flush=True)
    server.serve_forever()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the VibeStick telemetry service.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    run_server(args.host, args.port)


def _is_mutating_path(path: str) -> bool:
    return path in {
        "/telemetry/v1/samples",
        "/telemetry/v1/sessions",
    } or bool(_session_route(path, "stop"))


def _session_route(path: str, leaf: str) -> str:
    prefix = "/telemetry/v1/sessions/"
    if not path.startswith(prefix):
        return ""
    parts = path.removeprefix(prefix).strip("/").split("/")
    if not parts or not parts[0]:
        return ""
    if leaf == "":
        return parts[0] if len(parts) == 1 else ""
    return parts[0] if len(parts) == 2 and parts[1] == leaf else ""


def _device_raw_route(path: str, leaf: str) -> str:
    prefix = "/telemetry/v1/devices/"
    if not path.startswith(prefix):
        return ""
    parts = path.removeprefix(prefix).strip("/").split("/")
    expected = 2 if leaf == "" else 3
    if len(parts) != expected or parts[1] != "raw":
        return ""
    if leaf and parts[2] != leaf:
        return ""
    return unquote(parts[0])


def _dashboard_file(request_path: str) -> Path | None:
    root = Path(__file__).resolve().parents[1] / "web" / "telemetry"
    candidate = root / (
        "index.html" if request_path == "/telemetry/" else request_path.removeprefix("/telemetry/").strip("/")
    )
    try:
        resolved_root = root.resolve()
        resolved_candidate = candidate.resolve()
    except OSError:
        return None
    if resolved_root not in resolved_candidate.parents and resolved_candidate != resolved_root:
        return None
    return resolved_candidate if resolved_candidate.is_file() else None


def _telemetry_token() -> str:
    return os.environ.get(
        "VIBE_STICK_TELEMETRY_TOKEN",
        os.environ.get("VIBE_STICK_BRIDGE_TOKEN", ""),
    ).strip()


def _enforce_bind_security(host: str) -> None:
    normalized = host.strip("[]")
    try:
        address = ipaddress.ip_address(normalized)
    except ValueError:
        address = None
    if address is not None and address.is_loopback:
        return
    if _telemetry_token():
        return
    raise RuntimeError(
        "VIBE_STICK_TELEMETRY_TOKEN (or VIBE_STICK_BRIDGE_TOKEN) is required when binding telemetry outside loopback."
    )


def _first(query: dict[str, list[str]], key: str) -> str:
    values = query.get(key, [])
    return values[0] if values else ""


if __name__ == "__main__":
    main()
