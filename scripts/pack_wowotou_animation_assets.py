#!/usr/bin/env python3
"""Pack transparent Wowotou PNG frame sequences into a flash asset blob."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IN = ROOT / ".tmp" / "wowotou-frames-transparent"
DEFAULT_OUT = ROOT / "firmware" / "sticks3" / "generated" / "wowotou_anim_assets.bin"

MAGIC = b"VSA1"
WIDTH = 112
HEIGHT = 112
FPS = 10
ASSET_ENTRY_SIZE = 56
FRAME_ENTRY_SIZE = 8
BACKGROUND_RGB = (5, 6, 8)
EXCLUDED_ASSETS = {
    "cloudling-building",
    "cloudling-dozing-to-sleeping",
    "cloudling-idle-to-dozing",
    "cloudling-idle-to-sleeping",
    "cloudling-mini-alert",
    "cloudling-mini-enter-sleep",
    "cloudling-mini-happy",
    "cloudling-mini-idle",
    "cloudling-mini-peek",
    "cloudling-mini-sleep",
    "cloudling-mini-typing",
    "cloudling-sleeping",
}


def has_visible_pixels(image: Image.Image) -> bool:
    return image.convert("RGBA").getchannel("A").getbbox() is not None


def rgb565_words(image: Image.Image) -> list[int]:
    words = []
    for red, green, blue, alpha in image.convert("RGBA").getdata():
        if alpha == 0:
            red, green, blue = BACKGROUND_RGB
        elif alpha < 255:
            a = alpha / 255
            red = round(red * a + BACKGROUND_RGB[0] * (1 - a))
            green = round(green * a + BACKGROUND_RGB[1] * (1 - a))
            blue = round(blue * a + BACKGROUND_RGB[2] * (1 - a))
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


def align4(value: int) -> int:
    return (value + 3) & ~3


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack Wowotou frame directories into a firmware asset blob.")
    parser.add_argument("--input", type=Path, default=DEFAULT_IN)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    asset_dirs = sorted(path for path in args.input.iterdir() if path.is_dir())
    assets: list[tuple[str, list[bytes]]] = []
    for asset_dir in asset_dirs:
        if asset_dir.name in EXCLUDED_ASSETS:
            print(f"skip excluded asset: {asset_dir.name}")
            continue
        frames = sorted(asset_dir.glob("frame_*.png"))
        if not frames:
            raise SystemExit(f"{asset_dir.name}: no frames found")
        encoded = []
        for frame in frames:
            image = Image.open(frame)
            if image.size != (WIDTH, HEIGHT):
                raise SystemExit(f"{frame}: expected {WIDTH}x{HEIGHT}, got {image.size}")
            if not has_visible_pixels(image):
                continue
            encoded.append(encode_rle(rgb565_words(image)))
        if not encoded:
            print(f"skip empty asset: {asset_dir.name}")
            continue
        assets.append((asset_dir.name, encoded))

    header_size = 16
    asset_table_offset = header_size
    frame_table_offset = asset_table_offset + len(assets) * ASSET_ENTRY_SIZE
    data_offset = frame_table_offset + sum(len(frames) * FRAME_ENTRY_SIZE for _, frames in assets)
    data_offset = align4(data_offset)

    blob = bytearray(data_offset)
    struct.pack_into("<4sHHHHI", blob, 0, MAGIC, len(assets), WIDTH, HEIGHT, FPS, asset_table_offset)

    frame_cursor = frame_table_offset
    data_cursor = data_offset
    for asset_index, (name, frames) in enumerate(assets):
        name_bytes = name.encode("utf-8")
        if len(name_bytes) >= 40:
            raise SystemExit(f"asset name too long: {name}")
        entry_offset = asset_table_offset + asset_index * ASSET_ENTRY_SIZE
        blob[entry_offset : entry_offset + 40] = name_bytes + b"\0" * (40 - len(name_bytes))
        struct.pack_into("<III", blob, entry_offset + 40, len(frames), frame_cursor, 0)

        for frame in frames:
            data_cursor = align4(data_cursor)
            if len(blob) < data_cursor:
                blob.extend(b"\0" * (data_cursor - len(blob)))
            struct.pack_into("<II", blob, frame_cursor, data_cursor, len(frame))
            frame_cursor += FRAME_ENTRY_SIZE
            blob.extend(frame)
            data_cursor += len(frame)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"assets: {len(assets)}")
    print(f"frames: {sum(len(frames) for _, frames in assets)}")
    print(f"bytes: {len(blob)}")
    print(f"output: {args.output.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
