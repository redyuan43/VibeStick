#!/usr/bin/env python3
"""Generate the small, LVGL-free pet set used by the MiniJoy BT firmware."""

from __future__ import annotations

import re
from pathlib import Path

import generate_pet_assets as base
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
OUT_C = ROOT / "firmware" / "sticks3" / "generated" / "vibe_minijoy_pet_assets.c"
OUT_H = ROOT / "firmware" / "sticks3" / "generated" / "vibe_minijoy_pet_assets.h"
SOURCE_C = ROOT / "firmware" / "sticks3" / "generated" / "vibe_stick_pet_assets.c"

ASSET_SIZE = 80
ASSETS = [
    ("IDLE", "cloudling_idle"),
    ("BLINK_LEFT", "cloudling_idle_blink_left"),
    ("BLINK_RIGHT", "cloudling_idle_blink_right"),
    ("BLINK_BOTH", "cloudling_idle_blink_both"),
    ("ATTENTION", "cloudling_attention"),
    ("HAPPY", "cloudling_mini_happy"),
    ("ERROR", "cloudling_error"),
]


def array_symbol(name: str) -> str:
    return f"s_pet_frame_{name.lower()}"


def enum_name(name: str) -> str:
    return f"VIBE_MINIJOY_PET_FRAME_{name}"


def decode_rle(data: bytes, expected_pixels: int) -> list[int]:
    words: list[int] = []
    offset = 0
    while offset < len(data):
        control = data[offset]
        offset += 1
        count = (control & 0x7F) + 1
        if control & 0x80:
            if offset + 2 > len(data):
                raise ValueError("truncated RLE run")
            value = data[offset] | (data[offset + 1] << 8)
            offset += 2
            words.extend([value] * count)
        else:
            byte_count = count * 2
            if offset + byte_count > len(data):
                raise ValueError("truncated RLE literal")
            for _ in range(count):
                words.append(data[offset] | (data[offset + 1] << 8))
                offset += 2
    if len(words) != expected_pixels:
        raise ValueError(f"unexpected decoded pixel count: {len(words)}")
    return words


def load_main_frame(source_text: str, source_name: str) -> Image.Image:
    symbol = f"s_pet_frame_{source_name}"
    match = re.search(
        rf"static const uint8_t {re.escape(symbol)}\[\] = \{{(.*?)\n\}};",
        source_text,
        flags=re.S,
    )
    if not match:
        raise SystemExit(f"missing generated main pet frame: {symbol}")
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    pixels = []
    for word in decode_rle(data, 112 * 112):
        red = ((word >> 11) & 0x1F) * 255 // 31
        green = ((word >> 5) & 0x3F) * 255 // 63
        blue = (word & 0x1F) * 255 // 31
        pixels.append((red, green, blue))
    image = Image.new("RGB", (112, 112))
    image.putdata(pixels)
    return image.resize((ASSET_SIZE, ASSET_SIZE), Image.Resampling.LANCZOS)


def write_assets() -> None:
    header = [
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"#define VIBE_MINIJOY_PET_WIDTH {ASSET_SIZE}",
        f"#define VIBE_MINIJOY_PET_HEIGHT {ASSET_SIZE}",
        "",
        "typedef enum {",
    ]
    for name, _ in ASSETS:
        header.append(f"    {enum_name(name)},")
    header.extend(
        [
            "    VIBE_MINIJOY_PET_FRAME_COUNT,",
            "} vibe_minijoy_pet_frame_id_t;",
            "",
            "typedef struct {",
            "    const uint8_t *data;",
            "    size_t size;",
            "} vibe_minijoy_pet_rle_frame_t;",
            "",
            "const vibe_minijoy_pet_rle_frame_t *vibe_minijoy_pet_frame(",
            "    vibe_minijoy_pet_frame_id_t frame_id);",
            "",
        ]
    )

    source = ['#include "vibe_minijoy_pet_assets.h"', ""]
    compressed_size = 0
    source_text = SOURCE_C.read_text(encoding="utf-8")
    for name, source_name in ASSETS:
        words = base.rgb565_words(load_main_frame(source_text, source_name))
        data = base.encode_rle(words)
        if decode_rle(data, ASSET_SIZE * ASSET_SIZE) != words:
            raise ValueError(f"RLE round-trip failed: {name}")
        compressed_size += len(data)
        source.extend(
            [
                f"static const uint8_t {array_symbol(name)}[] = {{",
                base.c_array(data),
                "};",
                "",
            ]
        )

    source.extend(
        [
            "static const vibe_minijoy_pet_rle_frame_t s_pet_frames[] = {",
        ]
    )
    for name, _ in ASSETS:
        symbol = array_symbol(name)
        source.append(
            f"    [{enum_name(name)}] = {{{symbol}, sizeof({symbol})}},"
        )
    source.extend(
        [
            "};",
            "",
            "const vibe_minijoy_pet_rle_frame_t *vibe_minijoy_pet_frame(",
            "    vibe_minijoy_pet_frame_id_t frame_id)",
            "{",
            "    if ((int)frame_id < 0 || frame_id >= VIBE_MINIJOY_PET_FRAME_COUNT) {",
            "        return NULL;",
            "    }",
            "    return &s_pet_frames[frame_id];",
            "}",
            "",
        ]
    )

    OUT_H.write_text("\n".join(header), encoding="utf-8")
    OUT_C.write_text("\n".join(source), encoding="utf-8")
    raw_size = len(ASSETS) * ASSET_SIZE * ASSET_SIZE * 2
    print(f"frames: {len(ASSETS)}")
    print(f"raw bytes: {raw_size}")
    print(f"rle bytes: {compressed_size} ({compressed_size / raw_size:.1%})")
    print(f"generated {OUT_H.relative_to(ROOT)}")
    print(f"generated {OUT_C.relative_to(ROOT)}")


def main() -> None:
    write_assets()


if __name__ == "__main__":
    main()
