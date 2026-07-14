from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

DEFAULT_BASE_URL = "http://127.0.0.1:8765"


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    client = _Client(args.base_url, args.token or os.environ.get("VIBE_STICK_BRIDGE_TOKEN", ""))
    try:
        result = args.func(client, args)
    except _CliError as exc:
        print(exc, file=sys.stderr)
        return 1
    exit_code = 0
    if isinstance(result, dict) and "__exit_code" in result:
        exit_code = int(result.pop("__exit_code"))
    if result is not None:
        if isinstance(result, str):
            print(result, end="" if result.endswith("\n") else "\n")
        else:
            print(json.dumps(result, indent=2, sort_keys=True))
    return exit_code


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="VibeStick battery telemetry CLI.")
    parser.add_argument("--base-url", default=os.environ.get("VIBE_STICK_BRIDGE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--token", default="")
    sub = parser.add_subparsers(dest="command", required=True)

    devices = sub.add_parser("devices", help="List latest device telemetry.")
    devices.set_defaults(func=lambda client, args: client.get("/telemetry/v1/devices"))

    sessions = sub.add_parser("sessions", help="List battery test sessions.")
    sessions.set_defaults(func=lambda client, args: client.get("/telemetry/v1/sessions"))

    smoke = sub.add_parser("smoke", help="Create a 10 minute smoke battery session.")
    _add_run_arguments(smoke)
    smoke.add_argument("--label", default="")
    smoke.set_defaults(func=lambda client, args: _run_session(client, args, "smoke"))

    full = sub.add_parser("full", help="Create a full battery drain session.")
    _add_run_arguments(full)
    full.add_argument("--label", default="")
    full.add_argument("--allow-partial", action="store_true")
    full.add_argument(
        "--resume-unplugged",
        action="store_true",
        help="Attach to the confirmed unplug point in the current boot's raw telemetry.",
    )
    full.set_defaults(func=lambda client, args: _run_session(client, args, "full"))

    charge = sub.add_parser("charge", help="Create a battery charging session.")
    _add_run_arguments(charge)
    charge.add_argument("--label", default="")
    charge.set_defaults(func=lambda client, args: _run_session(client, args, "charge"))

    raw = sub.add_parser("raw", help="List always-on raw telemetry for a device.")
    raw.add_argument("device_id")
    raw.add_argument("--boot-id", default="")
    raw.set_defaults(
        func=lambda client, args: client.get(
            f"/telemetry/v1/devices/{quote(args.device_id, safe='')}/raw"
            + (f"?boot_id={quote(args.boot_id, safe='')}" if args.boot_id else "")
        )
    )

    raw_export = sub.add_parser("raw-export", help="Export always-on raw telemetry as CSV.")
    raw_export.add_argument("device_id")
    raw_export.add_argument("--boot-id", default="")
    raw_export.set_defaults(
        func=lambda client, args: client.get_text(
            f"/telemetry/v1/devices/{quote(args.device_id, safe='')}/raw/export.csv"
            + (f"?boot_id={quote(args.boot_id, safe='')}" if args.boot_id else "")
        )
    )

    show = sub.add_parser("show", help="Show one battery test session.")
    show.add_argument("session_id")
    show.set_defaults(
        func=lambda client, args: client.get(f"/telemetry/v1/sessions/{args.session_id}")
    )

    stop = sub.add_parser("stop", help="Stop a running battery session.")
    stop.add_argument("session_id")
    stop.add_argument("--reason", default="manual")
    stop.set_defaults(
        func=lambda client, args: client.post(
            f"/telemetry/v1/sessions/{args.session_id}/stop",
            {"reason": args.reason},
        )
    )

    export = sub.add_parser("export", help="Export session samples as CSV.")
    export.add_argument("session_id")
    export.set_defaults(func=lambda client, args: client.get_text(f"/telemetry/v1/sessions/{args.session_id}/export.csv"))

    return parser


def _session_payload(args: argparse.Namespace, kind: str) -> dict[str, Any]:
    return {
        "device_id": args.device_id,
        "kind": kind,
        "allow_partial": bool(getattr(args, "allow_partial", False)),
        "resume_unplugged": bool(getattr(args, "resume_unplugged", False)),
        "label": args.label,
    }


def _add_run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("device_id", nargs="?")
    parser.add_argument("--board", choices=("sticks3", "stickc_plus_11"))
    parser.add_argument("--detach", action="store_true")


def _run_session(client: _Client, args: argparse.Namespace, kind: str) -> dict[str, Any]:
    device_id = _resolve_device(client, args.device_id, args.board)
    args.device_id = device_id
    created = client.post("/telemetry/v1/sessions", _session_payload(args, kind))
    if args.detach:
        return created

    session = created["session"]
    session_id = str(session["id"])
    if kind == "charge":
        print(f"Charge session {session_id} is running for {device_id}. Keep USB connected.", flush=True)
    elif getattr(args, "resume_unplugged", False):
        print(f"Session {session_id} resumed from raw unplug telemetry for {device_id}.", flush=True)
    else:
        print(f"Session {session_id} is ready. Unplug USB power from {device_id} now.", flush=True)
    last_status = ""
    while True:
        payload = client.get(f"/telemetry/v1/sessions/{session_id}")
        session = payload["session"]
        status = str(session.get("status") or "")
        if status != last_status:
            print(f"Session status: {status}", flush=True)
            last_status = status
        if status not in {"waiting_for_unplug", "running"}:
            payload["__exit_code"] = 0 if status == "passed" else 1
            return payload
        time.sleep(2)


def _resolve_device(client: _Client, device_id: str | None, board: str | None) -> str:
    if device_id:
        return device_id
    if not board:
        raise _CliError("provide device_id or --board")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        devices = client.get("/telemetry/v1/devices").get("devices", [])
        matches = [item for item in devices if item.get("board") == board]
        if matches:
            matches.sort(key=lambda item: float(item.get("last_seen_epoch") or 0), reverse=True)
            return str(matches[0]["device_id"])
        time.sleep(2)
    raise _CliError(f"no {board} telemetry device appeared within 60 seconds")


class _Client:
    def __init__(self, base_url: str, token: str) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token

    def get(self, path: str) -> dict[str, Any]:
        return json.loads(self.get_text(path))

    def get_text(self, path: str) -> str:
        return self._request("GET", path, None).decode("utf-8")

    def post(self, path: str, payload: dict[str, Any]) -> dict[str, Any]:
        return json.loads(self._request("POST", path, json.dumps(payload).encode("utf-8")).decode("utf-8"))

    def _request(self, method: str, path: str, data: bytes | None) -> bytes:
        headers = {}
        if data is not None:
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["X-Vibe-Stick-Token"] = self.token
        request = Request(f"{self.base_url}{path}", data=data, headers=headers, method=method)
        try:
            with urlopen(request, timeout=10) as response:
                return response.read()
        except HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            raise _CliError(f"{method} {path} failed: HTTP {exc.code} {body}") from exc
        except URLError as exc:
            raise _CliError(f"{method} {path} failed: {exc.reason}") from exc


class _CliError(RuntimeError):
    pass


if __name__ == "__main__":
    raise SystemExit(main())
