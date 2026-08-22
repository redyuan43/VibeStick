#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "sticks3"

PURE_MODULES = (
    "vibe_air_mouse",
    "vibe_app_state",
    "vibe_bridge_profile_policy",
    "vibe_input_router",
    "vibe_motion_controller",
    "vibe_ota_policy",
    "vibe_power_policy",
    "vibe_power_runtime",
    "vibe_recording_policy",
    "vibe_recording_controller",
    "vibe_settings",
    "vibe_wifi_policy",
    "vibe_wav",
)

BANNED_TOKENS = (
    '#include "cJSON.h"',
    '#include "esp_',
    '#include "freertos/',
    '#include "lvgl.h"',
    '#include "nvs',
    '#include "lwip/',
    '#include "driver/',
)

MAIN_BANNED_TOKENS = (
    "cJSON",
    "nvs_",
    "lv_",
    "esp_http_client",
    "socket",
    "xTask",
    "xQueue",
)


def main() -> int:
    failures: list[str] = []
    for module in PURE_MODULES:
        for directory, suffix in (("include", ".h"), ("src", ".c")):
            path = FIRMWARE / directory / f"{module}{suffix}"
            source = path.read_text(encoding="utf-8")
            for token in BANNED_TOKENS:
                if token in source:
                    failures.append(f"{path.relative_to(ROOT)} imports {token}")

    entrypoint = FIRMWARE / "src" / "main.c"
    entry_source = entrypoint.read_text(encoding="utf-8")
    entry_lines = len(entry_source.splitlines())
    if entry_lines > 1500:
        failures.append(
            f"{entrypoint.relative_to(ROOT)} has {entry_lines} lines; expected <= 1500"
        )
    for token in MAIN_BANNED_TOKENS:
        if token in entry_source:
            failures.append(
                f"{entrypoint.relative_to(ROOT)} directly owns forbidden resource {token}"
            )

    if failures:
        print("Firmware architecture boundary violations:")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print(
        "Firmware architecture boundaries passed "
        f"({len(PURE_MODULES)} pure modules, {entry_lines}-line entrypoint)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
