#!/usr/bin/env python3
"""Render all Wowotou SVG assets into transparent PNG animation frames."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "sticks3" / "assets" / "wowotou"
DEFAULT_OUT = ROOT / ".tmp" / "wowotou-frames-transparent"

ASSET_SCALES = {
    "cloudling-mini-alert.svg": 1.65,
    "cloudling-mini-crabwalk.svg": 1.65,
    "cloudling-mini-enter-roll-in.svg": 1.65,
    "cloudling-mini-enter-sleep.svg": 1.65,
    "cloudling-mini-happy.svg": 1.65,
    "cloudling-mini-idle.svg": 1.65,
    "cloudling-mini-peek.svg": 1.65,
    "cloudling-mini-sleep.svg": 1.65,
    "cloudling-mini-typing.svg": 1.65,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render all Wowotou SVG assets into PNG frame directories.")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="Output root directory.")
    parser.add_argument("--size", default="112x112", help="Output size passed to render_svg_animation_frames.py.")
    parser.add_argument("--frames", type=int, default=20, help="Frames per asset.")
    parser.add_argument("--fps", type=float, default=10.0, help="Sampling FPS.")
    parser.add_argument("--background", default="transparent", help="transparent or hex color.")
    parser.add_argument("--mode", choices=("smil", "realtime"), default="realtime", help="Animation sampling mode.")
    parser.add_argument("--default-scale", type=float, default=1.70, help="Scale for non-mini assets.")
    parser.add_argument("--clean", action="store_true", help="Remove output directory before rendering.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.clean and args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True, exist_ok=True)

    renderer = ROOT / "scripts" / "render_svg_animation_frames.py"
    svg_paths = sorted(SOURCE_DIR.glob("*.svg"))
    if not svg_paths:
        raise SystemExit(f"no SVG assets found in {SOURCE_DIR}")

    for index, svg_path in enumerate(svg_paths, start=1):
        scale = ASSET_SCALES.get(svg_path.name, args.default_scale)
        out_dir = args.out / svg_path.stem
        command = [
            "python3",
            str(renderer),
            str(svg_path),
            "--out",
            str(out_dir),
            "--size",
            args.size,
            "--frames",
            str(args.frames),
            "--fps",
            str(args.fps),
            "--background",
            args.background,
            "--mode",
            args.mode,
            "--content-scale",
            str(scale),
        ]
        print(f"[{index:02d}/{len(svg_paths):02d}] {svg_path.name} scale={scale}")
        subprocess.run(command, check=True)

    manifest = args.out / "assets.txt"
    manifest.write_text("\n".join(path.stem for path in svg_paths) + "\n", encoding="utf-8")
    print(f"rendered {len(svg_paths)} asset(s) to {args.out}")
    print(f"manifest: {manifest}")


if __name__ == "__main__":
    main()
