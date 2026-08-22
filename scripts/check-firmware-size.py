#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BASELINE_PATH = ROOT / "firmware" / "sticks3" / "size-baseline.json"


def image_path(board: str) -> Path:
    return (
        ROOT
        / "firmware"
        / "sticks3"
        / f"build-{board}"
        / f"vibe_stick_{board}.bin"
    )


def main() -> int:
    baselines = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    parser = argparse.ArgumentParser(
        description="Compare a firmware image with its recorded baseline."
    )
    parser.add_argument("board", choices=sorted(baselines))
    args = parser.parse_args()

    image = image_path(args.board)
    if not image.exists():
        parser.error(f"firmware image not found: {image}")

    size = image.stat().st_size
    baseline = int(baselines[args.board]["baseline_size"])
    partition = int(baselines[args.board]["ota_partition_size"])
    delta = size - baseline
    remaining = partition - size
    sign = "+" if delta >= 0 else ""
    print(
        f"{args.board}: {size} bytes "
        f"(baseline {baseline}, delta {sign}{delta}, "
        f"OTA remaining {remaining})"
    )
    if remaining < 0:
        print(
            f"{args.board}: image exceeds OTA partition by {-remaining} bytes"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
