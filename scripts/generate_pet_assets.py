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


def render_sheet(tmp_dir: Path) -> Image.Image:
    rows = (len(ASSETS) + GRID_COLUMNS - 1) // GRID_COLUMNS
    width = GRID_COLUMNS * ASSET_SIZE
    height = rows * ASSET_SIZE
    cells = []
    for filename, scale in ASSETS:
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
