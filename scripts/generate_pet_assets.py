#!/usr/bin/env python3
"""Generate compact LVGL pet frames from the selected Wowotou SVG files."""

from __future__ import annotations

import re
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
GRID_COLUMNS = 7
CAPTURE_DELAY_MS = 2500
BACKGROUND_RGB = (5, 6, 8)
FOREGROUND_THRESHOLD = 18
TARGET_VISUAL_MAX = 92

ASSETS = [
    ("cloudling-attention.svg", 1.70),
    ("cloudling-building.svg", 1.70),
    ("cloudling-carrying.svg", 1.70),
    ("cloudling-conducting.svg", 1.70),
    ("cloudling-dozing-to-sleeping.svg", 1.70),
    ("cloudling-dozing.svg", 1.70),
    ("cloudling-error.svg", 1.70),
    ("cloudling-idle-reading.svg", 1.70),
    ("cloudling-idle-to-dozing.svg", 1.70),
    ("cloudling-idle-to-sleeping.svg", 1.70),
    ("cloudling-idle.svg", 1.70),
    ("cloudling-idle-blink-left.svg", 1.70),
    ("cloudling-idle-blink-right.svg", 1.70),
    ("cloudling-idle-blink-both.svg", 1.70),
    ("cloudling-juggling.svg", 1.70),
    ("cloudling-mini-alert.svg", 1.65),
    ("cloudling-mini-crabwalk.svg", 1.65),
    ("cloudling-mini-enter-roll-in.svg", 1.65),
    ("cloudling-mini-enter-sleep.svg", 1.65),
    ("cloudling-mini-happy.svg", 1.65),
    ("cloudling-mini-idle.svg", 1.65),
    ("cloudling-mini-peek.svg", 1.65),
    ("cloudling-mini-sleep.svg", 1.65),
    ("cloudling-mini-typing.svg", 1.65),
    ("cloudling-notification.svg", 1.70),
    ("cloudling-react-drag.svg", 1.70),
    ("cloudling-sleeping-to-idle.svg", 1.70),
    ("cloudling-sleeping.svg", 1.70),
    ("cloudling-sweeping.svg", 1.70),
    ("cloudling-thinking.svg", 1.70),
    ("cloudling-typing.svg", 1.70),
]

IDLE_BLINK_VARIANTS = {
    "cloudling-idle-blink-left.svg": "left",
    "cloudling-idle-blink-right.svg": "right",
    "cloudling-idle-blink-both.svg": "both",
}


def find_chromium() -> str:
    for name in ("chromium", "chromium-browser", "google-chrome"):
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit("chromium was not found; install Chromium or set it on PATH")


def enum_name(filename: str) -> str:
    stem = Path(filename).stem.upper()
    return "VIBE_STICK_PET_FRAME_" + re.sub(r"[^A-Z0-9]+", "_", stem).strip("_")


def data_symbol(filename: str) -> str:
    stem = Path(filename).stem.lower()
    return "s_pet_frame_" + re.sub(r"[^a-z0-9]+", "_", stem).strip("_")


def closed_eye_svg(side: str) -> str:
    left_eye = (
        '<path d="M6.45 11.58 C7.10 12.28 8.90 12.28 10.05 11.58" '
        'fill="none" stroke="#21170f" stroke-width="0.58" '
        'stroke-linecap="round" stroke-linejoin="round" opacity="0.98"/>'
    )
    right_eye = (
        '<path d="M13.95 11.58 C14.60 12.28 16.40 12.28 17.55 11.58" '
        'fill="none" stroke="#21170f" stroke-width="0.58" '
        'stroke-linecap="round" stroke-linejoin="round" opacity="0.98"/>'
    )
    open_left = """
        <ellipse cx="8.25" cy="11.55" rx="1.95" ry="2.55" fill="#21170f" opacity="0.98"/>
        <circle cx="7.55" cy="10.55" r="0.70" fill="#ffffff" opacity="0.92"/>
        <circle cx="8.35" cy="12.95" r="0.42" fill="#ffffff" opacity="0.86"/>"""
    open_right = """
        <ellipse cx="15.75" cy="11.55" rx="1.95" ry="2.55" fill="#21170f" opacity="0.98"/>
        <circle cx="15.05" cy="10.55" r="0.70" fill="#ffffff" opacity="0.92"/>
        <circle cx="15.85" cy="12.95" r="0.42" fill="#ffffff" opacity="0.86"/>"""
    left = left_eye if side in {"left", "both"} else open_left
    right = right_eye if side in {"right", "both"} else open_right
    return f"""
      <g id="__wowotou-face" pointer-events="none">
        {left}
        {right}
        <ellipse cx="5.80" cy="14.75" rx="1.50" ry="0.70" fill="#ff9fc2" opacity="0.34"/>
        <ellipse cx="18.20" cy="14.75" rx="1.50" ry="0.70" fill="#ff9fc2" opacity="0.34"/>
        <path d="M4.55 13.15 L2.85 12.92 M4.55 14.25 L2.72 14.32 M4.66 15.30 L3.05 15.78" stroke="#3a241b" stroke-width="0.32" stroke-linecap="round" opacity="0.82"/>
        <path d="M19.45 13.15 L21.15 12.92 M19.45 14.25 L21.28 14.32 M19.34 15.30 L20.95 15.78" stroke="#3a241b" stroke-width="0.32" stroke-linecap="round" opacity="0.82"/>
        <path d="M17.35 15.95 C16.72 15.18 15.55 15.56 15.56 16.55 C15.56 17.48 16.60 18.16 17.35 18.76 C18.10 18.16 19.14 17.48 19.14 16.55 C19.15 15.56 17.98 15.18 17.35 15.95 Z" fill="#ffc1d5" stroke="#3a241b" stroke-width="0.18" opacity="0.95"/>
        <g filter="url(#drop)">
          <path d="M17.75 6.05 L18.38 7.35 L19.82 7.55 L18.78 8.55 L19.03 9.98 L17.75 9.30 L16.47 9.98 L16.72 8.55 L15.68 7.55 L17.12 7.35 Z" fill="#ffd86e" stroke="#fff3bd" stroke-width="0.18" stroke-linejoin="round"/>
        </g>
      </g>"""


def source_for_asset(filename: str, tmp_dir: Path) -> Path:
    if filename not in IDLE_BLINK_VARIANTS:
        return SOURCE_DIR / filename

    base = (SOURCE_DIR / "cloudling-idle.svg").read_text(encoding="utf-8")
    base = base.replace('<g id="eye-group">', '<g id="eye-group" opacity="0">', 1)
    face = closed_eye_svg(IDLE_BLINK_VARIANTS[filename])
    base = re.sub(
        r'\s*<g id="__wowotou-face" pointer-events="none">.*?\n<script type',
        f"\n{face}\n\n<script type",
        base,
        count=1,
        flags=re.S,
    )
    target = tmp_dir / filename
    target.write_text(base, encoding="utf-8")
    return target


def render_sheet(tmp_dir: Path) -> Image.Image:
    rows = (len(ASSETS) + GRID_COLUMNS - 1) // GRID_COLUMNS
    width = GRID_COLUMNS * ASSET_SIZE
    height = rows * ASSET_SIZE
    cells = []
    for filename, scale in ASSETS:
        source = source_for_asset(filename, tmp_dir)
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
      background: #{BACKGROUND_RGB[0]:02x}{BACKGROUND_RGB[1]:02x}{BACKGROUND_RGB[2]:02x};
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
      background: #{BACKGROUND_RGB[0]:02x}{BACKGROUND_RGB[1]:02x}{BACKGROUND_RGB[2]:02x};
    }}
    object {{
      position: absolute;
      display: block;
      background: #{BACKGROUND_RGB[0]:02x}{BACKGROUND_RGB[1]:02x}{BACKGROUND_RGB[2]:02x};
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


def foreground_bbox(image: Image.Image) -> tuple[int, int, int, int] | None:
    pixels = image.load()
    xs = []
    ys = []
    for y in range(image.height):
        for x in range(image.width):
            red, green, blue = pixels[x, y]
            distance = (
                abs(red - BACKGROUND_RGB[0])
                + abs(green - BACKGROUND_RGB[1])
                + abs(blue - BACKGROUND_RGB[2])
            )
            if distance > FOREGROUND_THRESHOLD:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def normalize_frame(image: Image.Image) -> Image.Image:
    bbox = foreground_bbox(image)
    if bbox is None:
        return image
    content = image.crop(bbox)
    max_side = max(content.size)
    if max_side <= 0:
        return image
    scale = TARGET_VISUAL_MAX / max_side
    width = max(1, round(content.width * scale))
    height = max(1, round(content.height * scale))
    content = content.resize((width, height), Image.Resampling.LANCZOS)
    frame = Image.new("RGB", (ASSET_SIZE, ASSET_SIZE), BACKGROUND_RGB)
    frame.paste(content, ((ASSET_SIZE - width) // 2, (ASSET_SIZE - height) // 2))
    return frame


def rgb565_words(image: Image.Image) -> list[int]:
    words = []
    for red, green, blue in image.getdata():
        words.append(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3))
    return words


def encode_rle(words: list[int]) -> bytes:
    data = bytearray()
    index = 0
    while index < len(words):
        run_len = 1
        while index + run_len < len(words) and words[index + run_len] == words[index] and run_len < 128:
            run_len += 1
        if run_len >= 3:
            value = words[index]
            data.append(0x80 | (run_len - 1))
            data.append(value & 0xFF)
            data.append(value >> 8)
            index += run_len
            continue

        literal_start = index
        literal_len = 0
        while index < len(words) and literal_len < 128:
            next_run = 1
            while index + next_run < len(words) and words[index + next_run] == words[index] and next_run < 128:
                next_run += 1
            if next_run >= 3 and literal_len > 0:
                break
            index += 1
            literal_len += 1
            if next_run >= 3:
                break

        data.append(literal_len - 1)
        for value in words[literal_start : literal_start + literal_len]:
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
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "#include \"lvgl.h\"",
        "",
        f"#define VIBE_STICK_PET_WIDTH {ASSET_SIZE}",
        f"#define VIBE_STICK_PET_HEIGHT {ASSET_SIZE}",
        f"#define VIBE_STICK_PET_PIXEL_BYTES {ASSET_SIZE * ASSET_SIZE * 2}",
        "",
        "typedef enum {",
    ]
    for filename, _ in ASSETS:
        header_lines.append(f"    {enum_name(filename)},")
    header_lines.extend(
        [
            "    VIBE_STICK_PET_FRAME_COUNT,",
            "} vibe_stick_pet_frame_id_t;",
            "",
            "extern lv_image_dsc_t vibe_stick_pet_image;",
            "",
            "bool vibe_stick_pet_decode_frame(vibe_stick_pet_frame_id_t frame_id,",
            "                                 uint8_t *dest, size_t dest_size);",
            "void vibe_stick_pet_set_image_data(uint8_t *data);",
            "",
        ]
    )

    source_lines = [
        "#include \"vibe_stick_pet_assets.h\"",
        "",
    ]

    compressed_sizes = []
    for index, (filename, _) in enumerate(ASSETS):
        x = (index % GRID_COLUMNS) * ASSET_SIZE
        y = (index // GRID_COLUMNS) * ASSET_SIZE
        frame = normalize_frame(sheet.crop((x, y, x + ASSET_SIZE, y + ASSET_SIZE)))
        data = encode_rle(rgb565_words(frame))
        compressed_sizes.append(len(data))
        source_lines.extend(
            [
                f"static const uint8_t {data_symbol(filename)}[] = {{",
                c_array(data),
                "};",
                "",
            ]
        )

    source_lines.extend(
        [
            "typedef struct {",
            "    const uint8_t *data;",
            "    size_t size;",
            "} pet_rle_frame_t;",
            "",
            "static const pet_rle_frame_t s_pet_frames[] = {",
        ]
    )
    for filename, _ in ASSETS:
        symbol = data_symbol(filename)
        source_lines.append(f"    [{enum_name(filename)}] = {{{symbol}, sizeof({symbol})}},")
    source_lines.extend(
        [
            "};",
            "",
            "static uint8_t *s_pet_image_data;",
            "",
            "lv_image_dsc_t vibe_stick_pet_image = {",
            "    .header = {",
            "        .magic = LV_IMAGE_HEADER_MAGIC,",
            "        .cf = LV_COLOR_FORMAT_RGB565,",
            "        .flags = 0,",
            f"        .w = {ASSET_SIZE},",
            f"        .h = {ASSET_SIZE},",
            f"        .stride = {ASSET_SIZE * 2},",
            "        .reserved_2 = 0,",
            "    },",
            f"    .data_size = {ASSET_SIZE * ASSET_SIZE * 2},",
            "    .data = NULL,",
            "    .reserved = NULL,",
            "};",
            "",
            "void vibe_stick_pet_set_image_data(uint8_t *data)",
            "{",
            "    s_pet_image_data = data;",
            "    vibe_stick_pet_image.data = data;",
            "}",
            "",
            "bool vibe_stick_pet_decode_frame(vibe_stick_pet_frame_id_t frame_id,",
            "                                 uint8_t *dest, size_t dest_size)",
            "{",
            "    if ((int)frame_id < 0 || frame_id >= VIBE_STICK_PET_FRAME_COUNT ||",
            "        !dest || dest_size < VIBE_STICK_PET_PIXEL_BYTES) {",
            "        return false;",
            "    }",
            "    const pet_rle_frame_t *frame = &s_pet_frames[frame_id];",
            "    size_t in = 0;",
            "    size_t out = 0;",
            "    while (in < frame->size && out < VIBE_STICK_PET_PIXEL_BYTES) {",
            "        uint8_t control = frame->data[in++];",
            "        size_t count = (control & 0x7f) + 1;",
            "        if (control & 0x80) {",
            "            if (in + 2 > frame->size || out + count * 2 > VIBE_STICK_PET_PIXEL_BYTES) {",
            "                return false;",
            "            }",
            "            uint8_t lo = frame->data[in++];",
            "            uint8_t hi = frame->data[in++];",
            "            for (size_t i = 0; i < count; ++i) {",
            "                dest[out++] = lo;",
            "                dest[out++] = hi;",
            "            }",
            "        } else {",
            "            size_t bytes = count * 2;",
            "            if (in + bytes > frame->size || out + bytes > VIBE_STICK_PET_PIXEL_BYTES) {",
            "                return false;",
            "            }",
            "            for (size_t i = 0; i < bytes; ++i) {",
            "                dest[out++] = frame->data[in++];",
            "            }",
            "        }",
            "    }",
            "    return out == VIBE_STICK_PET_PIXEL_BYTES;",
            "}",
            "",
        ]
    )

    OUT_H.write_text("\n".join(header_lines), encoding="utf-8")
    OUT_C.write_text("\n".join(source_lines), encoding="utf-8")
    raw_size = len(ASSETS) * ASSET_SIZE * ASSET_SIZE * 2
    compressed_size = sum(compressed_sizes)
    ratio = compressed_size / raw_size
    print(f"frames: {len(ASSETS)}")
    print(f"raw bytes: {raw_size}")
    print(f"rle bytes: {compressed_size} ({ratio:.1%})")


def main() -> None:
    temp_root = ROOT / ".tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vibestick-pet-assets-", dir=temp_root) as tmp:
        write_assets(render_sheet(Path(tmp)))
    print(f"generated {OUT_H.relative_to(ROOT)}")
    print(f"generated {OUT_C.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
