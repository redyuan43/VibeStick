import importlib.util
import tempfile
import unittest
import wave
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "cyber_agent_omni.py"
SPEC = importlib.util.spec_from_file_location("cyber_agent_omni", SCRIPT_PATH)
assert SPEC is not None
omni = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(omni)


class CyberAgentOmniTests(unittest.TestCase):
    def test_extracts_complete_sse_payload(self) -> None:
        payload = omni._read_sse_complete(
            'event: heartbeat\n'
            'data: null\n\n'
            'event: complete\n'
            'data: ["system\\nuser\\nassistant\\n今晚宜稳。", {"url": "http://x/out.wav"}]\n\n'
        )

        self.assertEqual(payload[1]["url"], "http://x/out.wav")

    def test_extracts_last_assistant_reply(self) -> None:
        self.assertEqual(
            omni._assistant_reply("system\nx\nuser\ny\nassistant\n今晚宜稳。"),
            "今晚宜稳。",
        )

    def test_reads_stream_chunks(self) -> None:
        emitted = []

        def lines():
            yield "event: start\n"
            yield 'data: {"speaker":"Chelsie"}\n'
            yield "event: text\n"
            yield 'data: {"text":"今晚宜稳。","chunks":1}\n'
            yield "event: chunk\n"
            yield 'data: {"index":1,"total":1,"text":"今晚宜稳。","audio_url":"/outputs/a.wav"}\n'
            self.assertEqual(emitted[0]["audio_url"], "http://100.103.199.121:7866/outputs/a.wav")
            yield "event: complete\n"
            yield 'data: {"chunks":1}\n'

        text, chunks = omni._read_stream_response(
            lines(),
            {"session_id": "test-stream"},
            on_chunk=emitted.append,
        )

        self.assertEqual(text, "今晚宜稳。")
        self.assertEqual(
            chunks,
            [
                {
                    "text": "今晚宜稳。",
                    "audio_url": "http://100.103.199.121:7866/outputs/a.wav",
                    "index": "1",
                    "total": "1",
                }
            ],
        )

    def test_audio_path_prefers_public_url(self) -> None:
        self.assertEqual(
            omni._audio_path(
                {
                    "audio_file": "/tmp/local.wav",
                    "audio_url": "http://bridge.local/recording/source?session_id=1",
                }
            ),
            "http://bridge.local/recording/source?session_id=1",
        )

    def test_converts_24k_wav_to_device_wav(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.wav"
            target = Path(tmp) / "target.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(24000)
                wav.writeframes(b"\x00\x00" * 24000)

            omni._convert_wav_to_device_wav(source, target)

            with wave.open(str(target), "rb") as wav:
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.getframerate(), 16000)


if __name__ == "__main__":
    unittest.main()
