import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "transcribe_via_agx_asr.py"
SPEC = importlib.util.spec_from_file_location("transcribe_via_agx_asr", SCRIPT_PATH)
assert SPEC is not None
agx_asr = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(agx_asr)


class AgxAsrTranscriberScriptTests(unittest.TestCase):
    def test_extracts_text_from_capswriter_response(self) -> None:
        payload = {
            "text": "今天有没有什么不宜的事情？",
            "raw_asr_text": "今天有没有什么不宜的事情？",
        }

        self.assertEqual(agx_asr._extract_text(payload), "今天有没有什么不宜的事情？")

    def test_multipart_body_uses_capswriter_audio_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            audio = Path(tmp) / "sample.wav"
            audio.write_bytes(b"RIFFtest")

            with mock.patch.dict(os.environ, {}, clear=True):
                body = agx_asr._multipart_body("boundary-test", audio)

        self.assertIn(b'name="audio"; filename="sample.wav"', body)
        self.assertIn(b'name="use_vad"', body)
        self.assertIn(b'name="use_punc"', body)
        self.assertIn(b"RIFFtest", body)


if __name__ == "__main__":
    unittest.main()
