import os
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import wave
from pathlib import Path
from unittest.mock import patch

from vibe_stick.audio import recorder
from vibe_stick.audio.recorder import RecordingController
from vibe_stick.audio.transcriber import TranscriptionResult


class FakeMicRecorder:
    def __init__(self) -> None:
        self.started = False

    def start(self, session_id: str):
        self.started = True
        return (False, None, "Mac mic should not start")

    def stop(self):
        return None


class FailingTranscriber:
    def transcribe(self, session_payload, explicit_text=""):
        return TranscriptionResult(
            success=False,
            message="No transcription adapter configured",
            source="none",
        )


class RecordingControllerTests(unittest.TestCase):
    def test_sticks3_source_uses_device_pcm(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, patch.object(recorder, "RECORDINGS_DIR", Path(tmp)):
            controller = RecordingController(Path(tmp) / "recording.json")
            fake_mic = FakeMicRecorder()
            controller.audio_recorder = fake_mic

            with patch.object(recorder, "show_hud"):
                session = controller.start(
                    {
                        "audio_source": "sticks3_pcm",
                        "session_id": "abcdef123456",
                    }
                )

            self.assertFalse(fake_mic.started)
            self.assertEqual(session.audio_source, "sticks3_pcm")
            self.assertEqual(session.status, "recording")

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

    def test_cyber_intent_runs_agent_hook_without_paste(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, patch.object(recorder, "RECORDINGS_DIR", Path(tmp)):
            controller = RecordingController(Path(tmp) / "recording.json")
            controller.audio_recorder = FakeMicRecorder()

            def fake_agent(payload, timeout):
                self.assertEqual(payload["intent"], "cyber_fortune")
                self.assertEqual(payload["mode"], "FORT")
                self.assertEqual(payload["transcript"], "今天适合做什么")
                self.assertEqual(timeout, 180)
                return (True, "宜写代码，忌乱提交。", "/tmp/vibestick-tts.wav")

            with (
                patch.object(recorder, "show_hud"),
                patch.object(recorder, "hide_hud"),
                patch.object(recorder, "_run_cyber_agent", side_effect=fake_agent),
            ):
                controller.start(
                    {
                        "audio_source": "sticks3_pcm",
                        "session_id": "abcdef123456",
                        "intent": "cyber_fortune",
                        "mode": "FORT",
                    }
                )
                session = controller.stop(
                    {
                        "text": "今天适合做什么",
                        "paste": True,
                        "intent": "cyber_fortune",
                        "mode": "FORT",
                    }
                )

            self.assertEqual(session.status, "cyber_done")
            self.assertFalse(session.pasted)
            self.assertEqual(session.agent_text, "宜写代码，忌乱提交。")
            self.assertEqual(session.tts_audio_file, "/tmp/vibestick-tts.wav")
            self.assertEqual(session.mode, "FORT")

    def test_cyber_intent_runs_agent_when_asr_is_unconfigured(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, patch.object(recorder, "RECORDINGS_DIR", Path(tmp)):
            controller = RecordingController(Path(tmp) / "recording.json")
            controller.audio_recorder = FakeMicRecorder()
            controller.transcriber = FailingTranscriber()

            def fake_agent(payload, timeout):
                self.assertEqual(payload["intent"], "cyber_fortune")
                self.assertEqual(payload["mode"], "FORT")
                self.assertEqual(payload["transcript_error"], "No transcription adapter configured")
                self.assertTrue(Path(payload["audio_file"]).is_file())
                self.assertEqual(timeout, 180)
                return (True, "已收到语音。", "/tmp/vibestick-tts.wav")

            with (
                patch.object(recorder, "show_hud"),
                patch.object(recorder, "hide_hud"),
                patch.object(
                    recorder,
                    "_wav_metrics",
                    return_value=recorder.AudioMetrics(
                        duration_seconds=3.0,
                        audio_bytes=96000,
                        rms=2000.0,
                        ac_rms=1900.0,
                        speech_seconds=1.2,
                        speech_windows=12,
                    ),
                ),
                patch.object(recorder, "_run_cyber_agent", side_effect=fake_agent),
            ):
                controller.start(
                    {
                        "audio_source": "sticks3_pcm",
                        "session_id": "abcdef123456",
                        "intent": "cyber_fortune",
                        "mode": "FORT",
                    }
                )
                controller.attach_pcm(b"\x01\x00" * 32, session_id="abcdef123456", append=True)
                session = controller.stop(
                    {
                        "paste": True,
                        "intent": "cyber_fortune",
                        "mode": "FORT",
                    }
                )

            self.assertEqual(session.status, "cyber_done")
            self.assertFalse(session.pasted)
            self.assertEqual(session.agent_text, "已收到语音。")
            self.assertEqual(session.message, "Cyber agent completed")

    def test_command_hook_timeout_kills_child_process_group(self) -> None:
        marker = f"vibestick-hook-timeout-{uuid.uuid4().hex}"
        child_code = "import time; time.sleep(30)"
        parent_code = (
            "import subprocess, sys; "
            f"subprocess.run([sys.executable, '-c', {child_code!r}, {marker!r}])"
        )
        command = f"{sys.executable} -c {shlex.quote(parent_code)}"

        with patch.dict(os.environ, {"VIBE_STICK_TEST_HOOK": command}):
            ok, _, message = recorder._run_command_hook("VIBE_STICK_TEST_HOOK", {}, timeout=1)

        self.assertFalse(ok)
        self.assertIn("timed out", message)
        time.sleep(0.2)
        processes = subprocess.run(["ps", "-ef"], check=False, capture_output=True, text=True)
        self.assertNotIn(marker, processes.stdout)


if __name__ == "__main__":
    unittest.main()
