#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import time
from urllib.request import Request, urlopen


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send simulated VibeStick battery telemetry.")
    parser.add_argument(
        "--base-url",
        default=os.environ.get("VIBE_STICK_TELEMETRY_URL", "http://127.0.0.1:8878"),
    )
    parser.add_argument(
        "--token",
        default=os.environ.get(
            "VIBE_STICK_TELEMETRY_TOKEN",
            os.environ.get("VIBE_STICK_BRIDGE_TOKEN", ""),
        ),
    )
    parser.add_argument("--board", choices=("sticks3", "stickc_plus_11"), required=True)
    parser.add_argument("--device-id", default="")
    parser.add_argument("--phase", choices=("powered", "battery"), default="powered")
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0)
    parser.add_argument("--start-mv", type=int, default=4100)
    parser.add_argument("--drop-mv", type=int, default=1)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    device_id = args.device_id or f"sim-{args.board}"
    pmic = "m5pm1" if args.board == "sticks3" else "axp192"
    powered = args.phase == "powered"
    boot_id = f"sim-{int(time.time())}"

    for sequence in range(max(1, args.count)):
        battery_mv = max(2500, args.start_mv - sequence * args.drop_mv)
        payload = {
            "schema_version": 1,
            "device_id": device_id,
            "boot_id": boot_id,
            "board": args.board,
            "pmic": pmic,
            "firmware_version": "simulator",
            "sequence": sequence,
            "uptime_ms": sequence * 5000,
            "sample_interval_ms": 5000,
            "wifi_connected": True,
            "battery_mv": battery_mv,
            "battery_percent": None,
            "usb_mv": 5000 if powered else 0,
            "charging": powered,
            "usb_powered": powered,
        }
        data = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "X-Vibe-Stick-Firmware-Name": "vibestick-battery-telemetry-simulator",
            "X-Vibe-Stick-Firmware-Version": "0.1.0",
            "X-Vibe-Stick-Firmware-Transport": "HTTP",
        }
        if args.token:
            headers["X-Vibe-Stick-Token"] = args.token
        request = Request(
            f"{args.base_url.rstrip('/')}/telemetry/v1/samples",
            data=data,
            headers=headers,
            method="POST",
        )
        with urlopen(request, timeout=10) as response:
            result = json.loads(response.read().decode("utf-8"))
        print(
            json.dumps(
                {
                    "device_id": result["device"]["device_id"],
                    "sequence": sequence,
                    "battery_mv": battery_mv,
                    "phase": args.phase,
                },
                sort_keys=True,
            ),
            flush=True,
        )
        if args.interval > 0 and sequence + 1 < args.count:
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
