#!/usr/bin/env python3
"""Normalize rendered Wowotou PNG frames to a consistent visual size."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IN = ROOT / ".tmp" / "wowotou-frames-transparent"
WIDTH = 112
HEIGHT = 112


def visible_bbox(path: Path) -> tuple[int, int, int, int] | None:
    with Image.open(path) as image:
        return image.convert("RGBA").getchannel("A").getbbox()


def asset_scale(frames: list[Path], target_max: int) -> float:
    max_edge = 0
    for frame in frames:
        bbox = visible_bbox(frame)
        if not bbox:
            continue
        max_edge = max(max_edge, bbox[2] - bbox[0], bbox[3] - bbox[1])
    return target_max / max_edge if max_edge else 1.0


def normalize_frame(path: Path, scale: float) -> None:
    image = Image.open(path).convert("RGBA")
    bbox = image.getchannel("A").getbbox()
    if not bbox:
        return

    cropped = image.crop(bbox)
    new_size = (
        max(1, round(cropped.width * scale)),
        max(1, round(cropped.height * scale)),
    )
    resized = cropped.resize(new_size, Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    x = (WIDTH - resized.width) // 2
    y = (HEIGHT - resized.height) // 2
    canvas.alpha_composite(resized, (x, y))
    canvas.save(path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Normalize Wowotou frame visual sizes.")
    parser.add_argument("--input", type=Path, default=DEFAULT_IN)
    parser.add_argument("--target-max", type=int, default=76)
    args = parser.parse_args()

    for asset_dir in sorted(path for path in args.input.iterdir() if path.is_dir()):
        frames = sorted(asset_dir.glob("frame_*.png"))
        if not frames:
            continue
        scale = asset_scale(frames, args.target_max)
        for frame in frames:
            normalize_frame(frame, scale)
        print(f"{asset_dir.name}: scale={scale:.3f}")


if __name__ == "__main__":
    main()
