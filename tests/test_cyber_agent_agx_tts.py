import importlib.util
import tempfile
import unittest
import wave
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "cyber_agent_agx_tts.py"
SPEC = importlib.util.spec_from_file_location("cyber_agent_agx_tts", SCRIPT_PATH)
assert SPEC is not None
cyber_agent = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(cyber_agent)


class CyberAgentAgxTtsTests(unittest.TestCase):
    def test_answer_text_uses_fortune_mode(self) -> None:
        text = cyber_agent._answer_text(
            {
                "intent": "cyber_fortune",
                "transcript": "今天出门有什么注意的吗？",
            }
        )

        self.assertIn("提醒", text)
        self.assertIn("出门看天气", text)

    def test_qwen_payload_disables_thinking(self) -> None:
        payload = cyber_agent._qwen_payload(
            {
                "intent": "cyber_fortune",
                "mode": "FORT",
                "transcript": "今天出门有什么注意的吗？",
            }
        )

        self.assertEqual(payload["model"], "caps-voice-edit-qwen3-4b:latest")
        self.assertFalse(payload["chat_template_kwargs"]["enable_thinking"])
        self.assertIn("今天出门有什么注意的吗？", payload["messages"][1]["content"])

    def test_qwen_payload_uses_almanac_service(self) -> None:
        payload = cyber_agent._qwen_payload(
            {
                "intent": "cyber_almanac",
                "mode": "ALM",
                "transcript": "今天适合做什么？",
            }
        )

        self.assertIn("赛博老黄历", payload["messages"][1]["content"])
        self.assertIn("赛博老黄历助手", payload["messages"][0]["content"])

    def test_service_for_session_defaults_to_fortune(self) -> None:
        self.assertEqual(cyber_agent._service_for_session({"intent": "cyber_fortune"})["name"], "fortune")
        self.assertEqual(cyber_agent._service_for_session({"intent": "cyber_almanac"})["name"], "almanac")
        self.assertEqual(cyber_agent._service_for_session({"intent": "unknown"})["name"], "fortune")

    def test_extracts_visible_qwen_content(self) -> None:
        text = cyber_agent._extract_qwen_text(
            {
                "choices": [
                    {
                        "message": {
                            "content": "出门看天气，钥匙证件确认。",
                            "reasoning_content": "hidden",
                        }
                    }
                ]
            }
        )

        self.assertEqual(text, "出门看天气，钥匙证件确认。")

    def test_converts_24k_wav_to_device_wav(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.wav"
            target = Path(tmp) / "target.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(24000)
                wav.writeframes(b"\x00\x00" * 24000)

            cyber_agent._convert_wav_to_device_wav(source, target)

            with wave.open(str(target), "rb") as wav:
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.getframerate(), 16000)


if __name__ == "__main__":
    unittest.main()
