#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OTA_DIR = ROOT / "firmware" / "sticks3" / "ota"
BOARDS = ("sticks3", "stickc_plus", "stickc_plus_se", "cardputer_adv")
LIVE_FIELDS = ("version", "build_id", "size", "sha256", "elf_sha256")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_local(board: str) -> dict[str, object]:
    manifest_path = OTA_DIR / f"{board}.json"
    binary_path = OTA_DIR / f"{board}.bin"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    failures: list[str] = []
    if manifest.get("available") is not True:
        failures.append("available is not true")
    if manifest.get("board") != board:
        failures.append(f"board is {manifest.get('board')!r}")
    if manifest.get("file_name") != f"{board}.bin":
        failures.append(f"file_name is {manifest.get('file_name')!r}")
    if manifest.get("url") != f"/ota/bin?board={board}":
        failures.append(f"url is {manifest.get('url')!r}")
    size = binary_path.stat().st_size
    if manifest.get("size") != size:
        failures.append(
            f"size is {manifest.get('size')!r}, binary is {size}"
        )
    file_sha = sha256(binary_path)
    if manifest.get("file_sha256") != file_sha:
        failures.append("file_sha256 does not match binary")
    build_image = (
        ROOT
        / "firmware"
        / "sticks3"
        / f"build-{board}"
        / f"vibe_stick_{board}.bin"
    )
    if not build_image.exists() or sha256(build_image) != file_sha:
        failures.append("published binary does not match build output")
    for field in LIVE_FIELDS:
        if not manifest.get(field):
            failures.append(f"{field} is missing")
    if failures:
        raise ValueError(f"{board}: " + "; ".join(failures))
    return manifest


def load_live(base_url: str, board: str, timeout: float) -> dict[str, object]:
    query = urllib.parse.urlencode({"board": board})
    url = f"{base_url.rstrip('/')}/ota/manifest?{query}"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        if response.status != 200:
            raise ValueError(f"{board}: live HTTP {response.status}")
        return json.loads(response.read().decode("utf-8"))


def compare_live(
    board: str,
    local: dict[str, object],
    live: dict[str, object],
) -> None:
    failures = [
        f"{field}: local={local.get(field)!r} live={live.get(field)!r}"
        for field in LIVE_FIELDS
        if live.get(field) != local.get(field)
    ]
    if live.get("board") != board:
        failures.append(
            f"board: local={board!r} live={live.get('board')!r}"
        )
    if failures:
        raise ValueError(f"{board}: " + "; ".join(failures))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify local and live VibeStick OTA manifests."
    )
    parser.add_argument(
        "--base-url",
        default="http://192.168.31.225:8765",
        help="CapsWriter M5 bridge base URL.",
    )
    parser.add_argument(
        "--local-only",
        action="store_true",
        help="Skip the live CapsWriter query.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Per-request timeout in seconds.",
    )
    parser.add_argument(
        "boards",
        nargs="*",
        choices=BOARDS,
    )
    args = parser.parse_args()
    boards = args.boards or BOARDS

    try:
        for board in boards:
            local = load_local(board)
            print(
                f"{board}: local version={local['version']} "
                f"size={local['size']} build_id={local['build_id']}"
            )
            if args.local_only:
                continue
            live = load_live(args.base_url, board, args.timeout)
            compare_live(board, local, live)
            print(f"{board}: live manifest matches")
    except (
        OSError,
        ValueError,
        json.JSONDecodeError,
        urllib.error.URLError,
    ) as error:
        print(f"OTA verification failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
