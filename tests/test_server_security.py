import os
import threading
import unittest
from types import SimpleNamespace
from unittest import mock

from vibe_stick.server import app


class ServerSecurityTests(unittest.TestCase):
    def test_loopback_host_does_not_require_token(self) -> None:
        self.assertFalse(app._host_requires_token("127.0.0.1"))
        self.assertFalse(app._host_requires_token("localhost"))
        self.assertFalse(app._host_requires_token("::1"))

    def test_non_loopback_host_requires_token(self) -> None:
        self.assertTrue(app._host_requires_token("0.0.0.0"))
        self.assertTrue(app._host_requires_token(""))
        self.assertTrue(app._host_requires_token("192.168.1.10"))

    def test_placeholder_token_is_treated_as_missing(self) -> None:
        with mock.patch.dict(os.environ, {"VIBE_STICK_BRIDGE_TOKEN": "change-this-shared-token"}):
            self.assertEqual(app._bridge_token(), "")

    def test_real_token_is_used(self) -> None:
        with mock.patch.dict(os.environ, {"VIBE_STICK_BRIDGE_TOKEN": "abc123-secret"}):
            self.assertEqual(app._bridge_token(), "abc123-secret")

    def test_debug_redaction_helpers_preserve_empty_values(self) -> None:
        self.assertEqual(app._redacted_text("今天有没有什么不宜的事情？"), "[redacted]")
        self.assertEqual(app._redacted_text(""), "")
        self.assertEqual(app._redacted_path("/tmp/session.wav"), ".../session.wav")

    def test_loopback_ip_can_view_sensitive_debug(self) -> None:
        self.assertTrue(app._is_loopback_ip("127.0.0.1"))
        self.assertTrue(app._is_loopback_ip("::1"))
        self.assertFalse(app._is_loopback_ip("192.168.31.41"))

    def test_dashboard_renders_recording_debug_fields(self) -> None:
        class Store:
            def debug_snapshot(self, *, include_sensitive: bool) -> dict[str, object]:
                return {
                    "bridge": {
                        "token_required": True,
                        "asr_command_configured": True,
                        "cyber_agent_configured": True,
                        "agx_asr_url": "http://agx.taild500c8.ts.net:8001/api/asr/transcribe",
                    },
                    "recording": {
                        "status": "cyber_done",
                        "intent": "cyber_fortune",
                        "mode": "FORT",
                        "audio_source": "sticks3_pcm",
                        "transcript_source": "command",
                        "message": "Cyber agent completed",
                        "transcript": "今天有没有什么不宜的事情？",
                        "agent_text": "已收到",
                        "agent_source": "agx_tts",
                        "audio_file": "/tmp/in.wav",
                        "tts_audio_file": "/tmp/out.wav",
                    },
                    "devices": [],
                    "events": [],
                    "sensitive": include_sensitive,
                }

        body = app._dashboard_html(Store(), ("127.0.0.1", 8766), include_sensitive=True)

        self.assertIn("Latest Recording", body)
        self.assertIn("cyber_done", body)
        self.assertIn("今天有没有什么不宜的事情？", body)
        self.assertIn("Model Reply", body)
        self.assertIn("agx_tts", body)

    def test_device_row_renders_boot_wake_diagnostics(self) -> None:
        row = app._device_row(
            {
                "device_id": "stick-s3",
                "device_ip": "192.168.100.177",
                "board": "sticks3",
                "firmware_name": "vibestick",
                "firmware_version": "0.1.27",
                "reset_reason": "deep_sleep",
                "wake_cause": "ext0",
                "boot_count": "4",
                "pmic_wake": "0x00",
                "pmic_irq": "00/00/00",
                "pmic_timer": "00/0",
                "pmic_gpio_wake": "00/00",
                "wifi_ssid": "HANYUAN",
                "wifi_rssi": -42,
                "last_seen_text": "2026-07-15 16:00:00",
                "path": "/health",
            }
        )

        self.assertIn(
            "deep_sleep/ext0 #4 PMIC:0x00 IRQ:00/00/00 Timer:00/0 GPIO:00/00",
            row,
        )

    def test_device_recording_result_is_small_and_keeps_tts_signal(self) -> None:
        payload = app._device_recording_result(
            {
                "recording": {
                    "session_id": "abc123",
                    "status": "cyber_done",
                    "message": "Cyber agent completed",
                    "intent": "cyber_fortune",
                    "mode": "FORT",
                    "transcript_source": "command",
                    "transcript": "很长的转写文本不应该进入设备 stop 响应",
                    "agent_text": "很长的 agent 文本不应该进入设备 stop 响应",
                    "tts_audio_file": "/tmp/out.wav",
                },
                "state": {
                    "time": "18:58",
                    "wifi": True,
                    "ble": False,
                    "active_provider": "codex",
                    "alert": {"event_id": "", "type": "NONE", "message": ""},
                },
            }
        )

        encoded = app.json.dumps(payload, ensure_ascii=False).encode("utf-8")

        self.assertLess(len(encoded), 512)
        self.assertEqual(payload["recording"]["status"], "cyber_done")
        self.assertTrue(payload["recording"]["tts_available"])
        self.assertNotIn("transcript", payload["recording"])
        self.assertNotIn("agent_text", payload["recording"])

    def test_playback_ok_status_is_not_agent_status(self) -> None:
        self.assertFalse(app._is_agent_status("ok"))
        self.assertTrue(app._is_agent_status("RUNNING"))

    def test_followup_enter_injects_key_for_the_active_recording_session(self) -> None:
        injector = mock.Mock()
        injector.press_key.return_value = SimpleNamespace(success=True, message="Pressed enter")
        store = app.BridgeStateStore.__new__(app.BridgeStateStore)
        store._lock = threading.RLock()
        store._events = []
        store._state = app.default_state()
        store._manual_status_until = 0.0
        store.recording = SimpleNamespace(
            session=SimpleNamespace(session_id="session-1"),
            paste_injector=injector,
        )
        store._save_state_locked = mock.Mock()

        store.update_from_event(
            {
                "event": "button_followup_enter",
                "source": "sticks3",
                "session_id": "session-1",
            }
        )

        injector.press_key.assert_called_once_with("enter")
        self.assertEqual(store._events[-1]["event"], "followup_key_sent")

    def test_followup_key_ignores_a_stale_recording_session(self) -> None:
        injector = mock.Mock()
        store = app.BridgeStateStore.__new__(app.BridgeStateStore)
        store._lock = threading.RLock()
        store._events = []
        store._state = app.default_state()
        store._manual_status_until = 0.0
        store.recording = SimpleNamespace(
            session=SimpleNamespace(session_id="active-session"),
            paste_injector=injector,
        )
        store._save_state_locked = mock.Mock()

        store.update_from_event(
            {
                "event": "button_followup_escape",
                "source": "sticks3",
                "session_id": "stale-session",
            }
        )

        injector.press_key.assert_not_called()
        self.assertEqual(store._events[-1]["event"], "followup_key_ignored")

    def test_event_row_renders_playback_details(self) -> None:
        row = app._event_row(
            {
                "time": "2026-07-09 19:00:00",
                "event": "tts_played",
                "status": "ok",
                "session_id": "abc123",
                "message": "ESP_OK",
            }
        )

        self.assertIn("tts_played", row)
        self.assertIn(">ok<", row)
        self.assertIn("ESP_OK", row)


if __name__ == "__main__":
    unittest.main()
