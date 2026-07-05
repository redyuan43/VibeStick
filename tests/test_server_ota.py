import json
import tempfile
import unittest
from pathlib import Path

from vibe_stick.server import app


class ServerOtaTests(unittest.TestCase):
    def test_missing_manifest_reports_unavailable_for_known_board(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            payload = app._ota_manifest_payload(Path(tmp), "sticks3")

        self.assertFalse(payload["available"])
        self.assertEqual(payload["board"], "sticks3")

    def test_manifest_payload_adds_default_download_url(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ota_dir = root / "firmware" / "sticks3" / "ota"
            ota_dir.mkdir(parents=True)
            (ota_dir / "sticks3.json").write_text(
                json.dumps(
                    {
                        "available": True,
                        "board": "sticks3",
                        "version": "v1",
                        "build_id": "Jul  5 2026 12:34:56",
                        "size": 123,
                        "file_name": "sticks3.bin",
                    }
                )
            )

            payload = app._ota_manifest_payload(root, "sticks3")

        self.assertTrue(payload["available"])
        self.assertEqual(payload["url"], "/ota/bin?board=sticks3")

    def test_ota_binary_path_uses_manifest_file_basename(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ota_dir = root / "firmware" / "sticks3" / "ota"
            ota_dir.mkdir(parents=True)
            binary = ota_dir / "sticks3.bin"
            binary.write_bytes(b"firmware")
            (ota_dir / "sticks3.json").write_text(
                json.dumps(
                    {
                        "available": True,
                        "board": "sticks3",
                        "file_name": "../sticks3.bin",
                    }
                )
            )

            resolved = app._ota_binary_path(root, "sticks3")

        self.assertEqual(resolved, binary)

    def test_unknown_board_does_not_resolve_ota_binary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(app._ota_binary_path(Path(tmp), "../sticks3"))


if __name__ == "__main__":
    unittest.main()
