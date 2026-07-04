#!/usr/bin/env python3
"""Generate LVGL RGB565 pet assets from the selected Wowotou SVG files."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "firmware" / "sticks3" / "assets" / "wowotou"
OUT_C = ROOT / "firmware" / "sticks3" / "generated" / "vibe_stick_pet_assets.c"
OUT_H = ROOT / "firmware" / "sticks3" / "generated" / "vibe_stick_pet_assets.h"

ASSET_SIZE = 112
GRID_COLUMNS = 5
CAPTURE_DELAY_MS = 2500

ASSETS = [
    ("vibe_stick_pet_mini_idle", "cloudling-mini-idle.svg", 1.65),
    ("vibe_stick_pet_mini_happy", "cloudling-mini-happy.svg", 1.65),
    ("vibe_stick_pet_mini_alert", "cloudling-mini-alert.svg", 1.65),
    ("vibe_stick_pet_mini_typing", "cloudling-mini-typing.svg", 1.65),
    ("vibe_stick_pet_mini_sleep", "cloudling-mini-sleep.svg", 1.65),
]


def find_chromium() -> str:
    for name in ("chromium", "chromium-browser", "google-chrome"):
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit("chromium was not found; install Chromium or set it on PATH")


def render_sheet(tmp_dir: Path) -> Image.Image:
    rows = (len(ASSETS) + GRID_COLUMNS - 1) // GRID_COLUMNS
    width = GRID_COLUMNS * ASSET_SIZE
    height = rows * ASSET_SIZE
    cells = []
    for _, filename, scale in ASSETS:
        source = SOURCE_DIR / filename
        if not source.exists():
            raise SystemExit(f"missing source SVG: {source}")
        scaled = round(ASSET_SIZE * scale)
        offset = round((ASSET_SIZE - scaled) / 2)
        cells.append(
            f'<div class="cell"><object data="{source.as_uri()}" '
            f'type="image/svg+xml" style="width:{scaled}px;height:{scaled}px;'
            f'left:{offset}px;top:{offset}px"></object></div>'
        )

    html = tmp_dir / "pet-sheet.html"
    html.write_text(
        f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <style>
    html, body {{
      margin: 0;
      width: {width}px;
      height: {height}px;
      overflow: hidden;
      background: #050608;
    }}
    body {{
      display: grid;
      grid-template-columns: repeat({GRID_COLUMNS}, {ASSET_SIZE}px);
      grid-auto-rows: {ASSET_SIZE}px;
    }}
    .cell {{
      position: relative;
      overflow: hidden;
      width: {ASSET_SIZE}px;
      height: {ASSET_SIZE}px;
      background: #050608;
    }}
    object {{
      position: absolute;
      display: block;
      background: #050608;
    }}
  </style>
</head>
<body>
  {''.join(cells)}
</body>
</html>
""",
        encoding="utf-8",
    )
    screenshot = tmp_dir / "pet-sheet.png"
    subprocess.run(
        [
            find_chromium(),
            "--headless",
            "--no-sandbox",
            "--disable-gpu",
            "--allow-file-access-from-files",
            "--run-all-compositor-stages-before-draw",
            f"--timeout={CAPTURE_DELAY_MS}",
            f"--window-size={width},{height}",
            f"--screenshot={screenshot}",
            html.as_uri(),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return Image.open(screenshot).convert("RGB")


def rgb565_bytes(image: Image.Image) -> bytes:
    data = bytearray()
    for red, green, blue in image.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        data.append(value & 0xFF)
        data.append(value >> 8)
    return bytes(data)


def c_array(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def write_assets(sheet: Image.Image) -> None:
    header_lines = [
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
    ]
    source_lines = [
        '#include "vibe_stick_pet_assets.h"',
        "",
    ]

    for index, (symbol, _, _) in enumerate(ASSETS):
        x = (index % GRID_COLUMNS) * ASSET_SIZE
        y = (index // GRID_COLUMNS) * ASSET_SIZE
        frame = sheet.crop((x, y, x + ASSET_SIZE, y + ASSET_SIZE))
        data = rgb565_bytes(frame)

        header_lines.append(f"extern const lv_image_dsc_t {symbol};")
        source_lines.extend(
            [
                f"static const uint8_t {symbol}_data[] = {{",
                c_array(data),
                "};",
                "",
                f"const lv_image_dsc_t {symbol} = {{",
                "    .header = {",
                "        .magic = LV_IMAGE_HEADER_MAGIC,",
                "        .cf = LV_COLOR_FORMAT_RGB565,",
                "        .flags = 0,",
                f"        .w = {ASSET_SIZE},",
                f"        .h = {ASSET_SIZE},",
                f"        .stride = {ASSET_SIZE * 2},",
                "        .reserved_2 = 0,",
                "    },",
                f"    .data_size = sizeof({symbol}_data),",
                f"    .data = {symbol}_data,",
                "    .reserved = NULL,",
                "};",
                "",
            ]
        )

    OUT_H.write_text("\n".join(header_lines) + "\n", encoding="utf-8")
    OUT_C.write_text("\n".join(source_lines) + "\n", encoding="utf-8")


def main() -> None:
    temp_root = Path.home() / "tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vibestick-pet-assets-", dir=temp_root) as tmp:
        write_assets(render_sheet(Path(tmp)))
    print(f"generated {OUT_H.relative_to(ROOT)}")
    print(f"generated {OUT_C.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
