#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


BOARDS = {"sticks3", "stickc_plus"}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_image_path(board: str) -> Path:
    return repo_root() / "firmware" / "sticks3" / f"build-{board}" / f"vibe_stick_{board}.bin"


def ota_dir() -> Path:
    return repo_root() / "firmware" / "sticks3" / "ota"


def esptool_python() -> Path:
    candidates: list[Path] = []
    env_python = os.environ.get("IDF_PYTHON_ENV_PATH", "").strip()
    if env_python:
        candidates.append(Path(env_python) / "bin" / "python")
    candidates.extend(sorted(Path.home().glob(".espressif/python_env/idf*_py*_env/bin/python"), reverse=True))
    candidates.append(Path(sys.executable))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return Path(sys.executable)


def esptool_script() -> Path:
    env_script = os.environ.get("ESPTOOL_PY", "").strip()
    if env_script:
        return Path(env_script).expanduser()
    idf_path = os.environ.get("IDF_PATH", "").strip()
    roots = [Path(idf_path)] if idf_path else []
    roots.append(Path.home() / "esp" / "esp-idf")
    for root in roots:
        script = root / "components" / "esptool_py" / "esptool" / "esptool.py"
        if script.exists():
            return script
    raise SystemExit("Could not find esptool.py. Source ESP-IDF export.sh or set ESPTOOL_PY.")


def image_info(image: Path) -> dict[str, str]:
    cmd = [str(esptool_python()), str(esptool_script()), "image_info", "--version", "2", str(image)]
    result = subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    info: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip().lower().replace(" ", "_")
        info[key] = value.strip()
    return info


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def firmware_build_id(image: Path, fallback: str) -> str:
    data = image.read_bytes()
    pattern = rb"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}"
    matches = re.findall(pattern, data)
    if not matches:
        return fallback
    return matches[-1].decode("ascii")


def firmware_version(board: str) -> str:
    config_path = repo_root() / "firmware" / "sticks3" / "include" / "vibe_stick_config.h"
    try:
        text = config_path.read_text()
    except OSError:
        return ""
    macro = {
        "sticks3": "VIBE_STICK_FIRMWARE_VERSION_STICKS3",
        "stickc_plus": "VIBE_STICK_FIRMWARE_VERSION_STICKC_PLUS",
    }[board]
    match = re.search(rf'#define\s+{macro}\s+"([^"]+)"', text)
    return match.group(1) if match else ""


def publish(board: str, image: Path) -> Path:
    if board not in BOARDS:
        raise SystemExit(f"Unsupported board: {board}")
    if not image.exists():
        raise SystemExit(f"Firmware image not found: {image}")

    info = image_info(image)
    target_dir = ota_dir()
    target_dir.mkdir(parents=True, exist_ok=True)
    file_name = f"{board}.bin"
    target_image = target_dir / file_name
    shutil.copy2(image, target_image)
    file_sha256 = sha256(target_image)
    image_sha256 = info.get("validation_hash", "").split()[0] or file_sha256
    compile_time = info.get("compile_time", "")
    build_id = firmware_build_id(target_image, compile_time)
    elf_sha256 = info.get("elf_file_sha256", "").split()[0]
    version = firmware_version(board) or info.get("app_version", "")

    manifest = {
        "available": True,
        "board": board,
        "version": version,
        "build_id": build_id,
        "project_name": info.get("project_name", ""),
        "idf_version": info.get("esp-idf", ""),
        "size": target_image.stat().st_size,
        "sha256": image_sha256,
        "elf_sha256": elf_sha256,
        "file_sha256": file_sha256,
        "file_name": file_name,
        "url": f"/ota/bin?board={board}",
    }
    manifest_path = target_dir / f"{board}.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    return manifest_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Publish a built VibeStick firmware image for Wi-Fi OTA.")
    parser.add_argument("board", choices=sorted(BOARDS))
    parser.add_argument("--image", type=Path, help="Path to a built .bin image. Defaults to build-<board> output.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    image = args.image or default_image_path(args.board)
    manifest_path = publish(args.board, image)
    print(f"Published OTA manifest: {manifest_path}")
    print(f"Published OTA image: {ota_dir() / (args.board + '.bin')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
