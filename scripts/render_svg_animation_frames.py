#!/usr/bin/env python3
"""Render an animated SVG into a PNG frame sequence on the host machine.

The firmware should not parse or play dynamic SVG directly. This tool samples
SMIL animation on the host and emits frame_000.png, frame_001.png, ... so the
device can play simple bitmap frames.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NODE_MODULES = ROOT / ".tmp" / "svg-render-node" / "node_modules"


def find_chrome() -> str:
    for name in ("google-chrome", "chromium", "chromium-browser"):
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit("Chrome/Chromium was not found; install one or add it to PATH")


def parse_size(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"(\d+)x(\d+)", value.strip())
    if not match:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT, for example 112x112")
    width = int(match.group(1))
    height = int(match.group(2))
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("size dimensions must be positive")
    return width, height


def parse_background(value: str) -> str:
    if value == "transparent":
        return value
    if not re.fullmatch(r"#?[0-9a-fA-F]{6}", value.strip()):
        raise argparse.ArgumentTypeError("background must be transparent or a hex color like #050608")
    return value if value.startswith("#") else f"#{value}"


def build_html(svg_text: str, width: int, height: int, background: str, content_scale: float) -> str:
    scaled_width = round(width * content_scale)
    scaled_height = round(height * content_scale)
    return f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <style>
    html, body {{
      margin: 0;
      width: {width}px;
      height: {height}px;
      overflow: hidden;
      background: {background};
    }}
    #capture-stage {{
      width: {width}px;
      height: {height}px;
      display: flex;
      align-items: center;
      justify-content: center;
      background: {background};
    }}
    svg {{
      width: {scaled_width}px !important;
      height: {scaled_height}px !important;
      max-width: {scaled_width}px;
      max-height: {scaled_height}px;
      flex: 0 0 auto;
    }}
  </style>
</head>
<body>
  <div id="capture-stage">
{svg_text}
  </div>
  <script>
    window.__set_svg_time = async (seconds) => {{
      const svg = document.querySelector("svg");
      if (svg && svg.pauseAnimations && svg.setCurrentTime) {{
        if (svg.unpauseAnimations) svg.unpauseAnimations();
        svg.setCurrentTime(seconds);
        svg.pauseAnimations();
      }}
      await new Promise((resolve) => requestAnimationFrame(resolve));
      await new Promise((resolve) => requestAnimationFrame(resolve));
      return true;
    }};
  </script>
</body>
</html>
"""


def render_with_playwright(
    html_path: Path,
    out_dir: Path,
    width: int,
    height: int,
    frames: int,
    fps: float,
    chrome: str,
    node_modules: Path,
    transparent: bool,
    mode: str,
    tmp_dir: Path,
) -> None:
    helper = tmp_dir / "render_frames.cjs"
    helper.write_text(
        """
const { chromium } = require('playwright-core');

(async () => {
  const options = JSON.parse(process.argv[2]);
  const browser = await chromium.launch({
    executablePath: options.chrome,
    headless: true,
    args: ['--no-sandbox', '--disable-gpu', '--allow-file-access-from-files']
  });
  const page = await browser.newPage({
    viewport: { width: options.width, height: options.height },
    deviceScaleFactor: 1
  });
  await page.goto(options.htmlUrl, { waitUntil: 'load' });
  const startedAt = Date.now();
  for (let i = 0; i < options.frames; i += 1) {
    const seconds = i / options.fps;
    if (options.mode === 'realtime') {
      const targetMs = Math.round(seconds * 1000);
      const elapsedMs = Date.now() - startedAt;
      if (targetMs > elapsedMs) {
        await page.waitForTimeout(targetMs - elapsedMs);
      }
      await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(resolve)));
    } else {
      await page.evaluate((value) => window.__set_svg_time(value), seconds);
    }
    await page.screenshot({
      path: `${options.outDir}/frame_${String(i).padStart(3, '0')}.png`,
      omitBackground: options.transparent
    });
  }
  await browser.close();
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
""",
        encoding="utf-8",
    )
    env = os.environ.copy()
    env["NODE_PATH"] = str(node_modules)
    payload = {
        "htmlUrl": html_path.as_uri(),
        "outDir": str(out_dir),
        "width": width,
        "height": height,
        "frames": frames,
        "fps": fps,
        "chrome": chrome,
        "transparent": transparent,
        "mode": mode,
    }
    subprocess.run(["node", str(helper), json.dumps(payload)], check=True, env=env)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Render an animated SVG into frame_000.png, frame_001.png, ..."
    )
    parser.add_argument("svg", type=Path, help="Input SVG file. SMIL animation is sampled with setCurrentTime().")
    parser.add_argument("--out", type=Path, required=True, help="Output directory for PNG frames.")
    parser.add_argument("--size", type=parse_size, default=(112, 112), help="Output size, default: 112x112.")
    parser.add_argument("--frames", type=int, default=60, help="Number of frames to render, default: 60.")
    parser.add_argument("--fps", type=float, default=30.0, help="Sampling frame rate, default: 30.")
    parser.add_argument("--content-scale", type=float, default=1.0, help="Scale the SVG content before capture.")
    parser.add_argument(
        "--mode",
        choices=("smil", "realtime"),
        default="smil",
        help="smil uses SVG setCurrentTime(); realtime waits between screenshots for JS/CSS animations.",
    )
    parser.add_argument(
        "--background",
        type=parse_background,
        default="transparent",
        help="transparent or hex color such as #050608. Default: transparent.",
    )
    parser.add_argument("--chrome", default=find_chrome(), help="Chrome/Chromium executable path.")
    parser.add_argument(
        "--node-modules",
        type=Path,
        default=DEFAULT_NODE_MODULES,
        help="Path containing playwright-core. Default: .tmp/svg-render-node/node_modules",
    )
    args = parser.parse_args()

    if args.frames <= 0:
        raise SystemExit("--frames must be positive")
    if args.fps <= 0:
        raise SystemExit("--fps must be positive")
    if args.content_scale <= 0:
        raise SystemExit("--content-scale must be positive")
    if not args.svg.exists():
        raise SystemExit(f"input SVG not found: {args.svg}")
    if not (args.node_modules / "playwright-core").exists():
        raise SystemExit(
            f"playwright-core not found in {args.node_modules}; "
            "run: npm --prefix .tmp/svg-render-node install playwright-core"
        )

    width, height = args.size
    args.out.mkdir(parents=True, exist_ok=True)
    svg_text = args.svg.read_text(encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix=".render-", dir=args.out) as tmp:
        tmp_dir = Path(tmp)
        html_path = tmp_dir / "animation.html"
        html_path.write_text(
            build_html(svg_text, width, height, args.background, args.content_scale),
            encoding="utf-8",
        )
        render_with_playwright(
            html_path,
            args.out,
            width,
            height,
            args.frames,
            args.fps,
            args.chrome,
            args.node_modules,
            args.background == "transparent",
            args.mode,
            tmp_dir,
        )

    manifest = args.out / "frames.txt"
    manifest.write_text(
        "\n".join(f"frame_{index:03d}.png" for index in range(args.frames)) + "\n",
        encoding="utf-8",
    )
    print(f"rendered {args.frames} frame(s) to {args.out}")
    print(f"manifest: {manifest}")


if __name__ == "__main__":
    main()
