import hashlib
import importlib.util
import struct
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/patch-sticks3-bridge-image.py"
spec = importlib.util.spec_from_file_location("recovery", SCRIPT)
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


def firmware(board):
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 2
    header[12] = 9
    header[23] = 1
    rodata = bytearray(48)
    rodata[16:23] = b"0.1.86\0"
    rodata += ("vibe_stick_" + board).encode() + b"\0sticks3_min\0"
    rodata += b"0.1.86\0" + b"192.168.100.142\0" + b"a" * 64 + b"\0X-Vibe-Stick-Token\0"
    code = bytes(range(128))
    image = header + struct.pack("<II", 0x3C000020, len(rodata)) + rodata
    image += struct.pack("<II", 0x42000020, len(code)) + code
    checksum = 0xEF
    for byte in rodata + code:
        checksum ^= byte
    image += b"\0" * ((len(image) | 15) - len(image)) + bytes([checksum])
    image += hashlib.sha256(image).digest()
    return bytes(image)


def patch(image, board):
    return recovery.patch(image, expected_sha256=hashlib.sha256(image).hexdigest(),
                          old_host="192.168.100.142", new_host="192.168.100.109",
                          old_version="0.1.86", new_version="0.1.87",
                          token="test_token_1234567890", board=board)


@pytest.mark.parametrize("board", ["sticks3", "cardputer_adv"])
def test_recovery_preserves_code_and_validates_updated_image(board):
    original = firmware(board)
    result, audit = patch(original, board)
    segments, _, _ = recovery.layout(result)
    for _, begin, end in segments[1:]:
        assert result[begin:end] == original[begin:end]
    assert audit["board"] == board
    assert audit["non_rodata_segments_unchanged"]
    assert b"192.168.100.109\0" in result
    assert b"test_token_1234567890\0" in result
    assert result.count(b"0.1.87\0") == 2


def test_recovery_rejects_wrong_board():
    with pytest.raises(ValueError, match="selected board"):
        patch(firmware("cardputer_adv"), "sticks3")


def test_recovery_rejects_corrupt_code():
    image = bytearray(firmware("cardputer_adv"))
    image[-50] ^= 1
    with pytest.raises(ValueError, match="checksum|digest"):
        patch(bytes(image), "cardputer_adv")
