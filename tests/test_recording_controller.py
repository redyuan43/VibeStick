import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

from vibe_stick.audio import recorder
from vibe_stick.audio.recorder import RecordingController


class FakeMicRecorder:
    def __init__(self) -> None:
        self.started = False

    def start(self, session_id: str):
        self.started = True
        return (False, None, "Mac mic should not start")

    def stop(self):
        return None


class RecordingControllerTests(unittest.TestCase):
    def test_stickc_plus_source_uses_device_pcm(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, patch.object(recorder, "RECORDINGS_DIR", Path(tmp)):
            controller = RecordingController(Path(tmp) / "recording.json")
            fake_mic = FakeMicRecorder()
            controller.audio_recorder = fake_mic

            with patch.object(recorder, "show_hud"):
                session = controller.start(
                    {
                        "audio_source": "stickc_plus_pcm",
                        "session_id": "abcdef123456",
                    }
                )

            self.assertFalse(fake_mic.started)
            self.assertEqual(session.audio_source, "stickc_plus_pcm")
            self.assertEqual(session.status, "recording")

    def test_append_pcm_upload_is_finalized_to_wav_on_stop(self) -> None:
        pcm_a = b"\x00\x00\x01\x00"
        pcm_b = b"\x02\x00\x03\x00"
        with tempfile.TemporaryDirectory() as tmp, patch.object(recorder, "RECORDINGS_DIR", Path(tmp)):
            controller = RecordingController(Path(tmp) / "recording.json")
            controller.audio_recorder = FakeMicRecorder()
            with patch.object(recorder, "show_hud"), patch.object(recorder, "hide_hud"):
                controller.start({"audio_source": "stickc_plus_pcm", "session_id": "abcdef123456"})
                controller.attach_pcm(pcm_a, session_id="abcdef123456", append=True)
                controller.attach_pcm(pcm_b, session_id="abcdef123456", append=True)
                session = controller.stop({"text": "ok", "paste": False})

            wav_path = Path(session.audio_file)
            self.assertTrue(wav_path.exists())
            self.assertFalse((Path(tmp) / "abcdef123456.pcm").exists())
            with wave.open(str(wav_path), "rb") as wav:
                self.assertEqual(wav.getframerate(), 16000)
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.readframes(4), pcm_a + pcm_b)


if __name__ == "__main__":
    unittest.main()
