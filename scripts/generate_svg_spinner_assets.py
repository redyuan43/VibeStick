#!/usr/bin/env python3
"""Generate RGB565/RLE LVGL assets from rendered SVG animation PNG frames."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
OUT_C = ROOT / "firmware" / "sticks3" / "generated" / "vibe_stick_svg_spinner_assets.c"
OUT_H = ROOT / "firmware" / "sticks3" / "generated" / "vibe_stick_svg_spinner_assets.h"
ASSET_SIZE = 112


def frame_index(path: Path) -> int:
    match = re.fullmatch(r"frame_(\d+)\.png", path.name)
    if not match:
        raise ValueError(f"unexpected frame name: {path.name}")
    return int(match.group(1))


def rgb565_words(image: Image.Image) -> list[int]:
    words = []
    for red, green, blue in image.convert("RGB").getdata():
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


def load_frames(frame_dir: Path) -> list[bytes]:
    paths = sorted(frame_dir.glob("frame_*.png"), key=frame_index)
    if not paths:
        raise SystemExit(f"no frame_*.png files found in {frame_dir}")

    frames = []
    for expected, path in enumerate(paths):
        if frame_index(path) != expected:
            raise SystemExit(f"missing frame_{expected:03d}.png before {path.name}")
        image = Image.open(path).convert("RGB")
        if image.size != (ASSET_SIZE, ASSET_SIZE):
            raise SystemExit(f"{path} must be {ASSET_SIZE}x{ASSET_SIZE}, got {image.size}")
        frames.append(encode_rle(rgb565_words(image)))
    return frames


def write_assets(frames: list[bytes]) -> None:
    header = [
        "#pragma once",
        "",
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "#include \"lvgl.h\"",
        "",
        f"#define VIBE_STICK_SVG_SPINNER_WIDTH {ASSET_SIZE}",
        f"#define VIBE_STICK_SVG_SPINNER_HEIGHT {ASSET_SIZE}",
        f"#define VIBE_STICK_SVG_SPINNER_PIXEL_BYTES {ASSET_SIZE * ASSET_SIZE * 2}",
        f"#define VIBE_STICK_SVG_SPINNER_FRAME_COUNT {len(frames)}",
        "",
        "extern lv_image_dsc_t vibe_stick_svg_spinner_image;",
        "",
        "bool vibe_stick_svg_spinner_decode_frame(int frame_id, uint8_t *dest, size_t dest_size);",
        "void vibe_stick_svg_spinner_set_image_data(uint8_t *data);",
        "",
    ]

    source = [
        "#include \"vibe_stick_svg_spinner_assets.h\"",
        "",
    ]
    for index, data in enumerate(frames):
        source.extend(
            [
                f"static const uint8_t s_svg_spinner_frame_{index:03d}[] = {{",
                c_array(data),
                "};",
                "",
            ]
        )

    source.extend(
        [
            "typedef struct {",
            "    const uint8_t *data;",
            "    size_t size;",
            "} svg_spinner_rle_frame_t;",
            "",
            "static const svg_spinner_rle_frame_t s_svg_spinner_frames[] = {",
        ]
    )
    for index in range(len(frames)):
        source.append(
            f"    {{s_svg_spinner_frame_{index:03d}, sizeof(s_svg_spinner_frame_{index:03d})}},"
        )
    source.extend(
        [
            "};",
            "",
            "static uint8_t *s_svg_spinner_image_data;",
            "",
            "lv_image_dsc_t vibe_stick_svg_spinner_image = {",
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
            "void vibe_stick_svg_spinner_set_image_data(uint8_t *data)",
            "{",
            "    s_svg_spinner_image_data = data;",
            "    vibe_stick_svg_spinner_image.data = data;",
            "}",
            "",
            "bool vibe_stick_svg_spinner_decode_frame(int frame_id, uint8_t *dest, size_t dest_size)",
            "{",
            "    if (frame_id < 0 || frame_id >= VIBE_STICK_SVG_SPINNER_FRAME_COUNT ||",
            "        !dest || dest_size < VIBE_STICK_SVG_SPINNER_PIXEL_BYTES) {",
            "        return false;",
            "    }",
            "    const svg_spinner_rle_frame_t *frame = &s_svg_spinner_frames[frame_id];",
            "    size_t in = 0;",
            "    size_t out = 0;",
            "    while (in < frame->size && out < VIBE_STICK_SVG_SPINNER_PIXEL_BYTES) {",
            "        uint8_t control = frame->data[in++];",
            "        size_t count = (control & 0x7f) + 1;",
            "        if (control & 0x80) {",
            "            if (in + 2 > frame->size || out + count * 2 > VIBE_STICK_SVG_SPINNER_PIXEL_BYTES) {",
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
            "            if (in + bytes > frame->size || out + bytes > VIBE_STICK_SVG_SPINNER_PIXEL_BYTES) {",
            "                return false;",
            "            }",
            "            for (size_t i = 0; i < bytes; ++i) {",
            "                dest[out++] = frame->data[in++];",
            "            }",
            "        }",
            "    }",
            "    return out == VIBE_STICK_SVG_SPINNER_PIXEL_BYTES;",
            "}",
            "",
        ]
    )

    OUT_H.write_text("\n".join(header), encoding="utf-8")
    OUT_C.write_text("\n".join(source), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate SVG spinner firmware assets from PNG frames.")
    parser.add_argument("frame_dir", type=Path, help="Directory containing frame_000.png, frame_001.png, ...")
    args = parser.parse_args()

    frames = load_frames(args.frame_dir)
    write_assets(frames)
    print(f"frames: {len(frames)}")
    print(f"rle bytes: {sum(len(frame) for frame in frames)}")
    print(f"generated {OUT_H.relative_to(ROOT)}")
    print(f"generated {OUT_C.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
