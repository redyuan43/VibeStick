from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = ROOT / "firmware" / "sticks3" / "src" / "main.c"


def test_front_single_click_toggles_device_recording() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_EVENT_RECORDING_TOGGLE" in source
    assert "queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);" in source
    assert 'handle_recording_start("button_tap_start", "再按发送")' in source
    assert 'handle_recording_stop("button_tap_stop")' in source
    assert 'post_simple_event("button_short", NULL)' not in source


def test_tap_recording_uses_existing_external_pcm_upload_path() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "start_recording_upload_task();" in source
    assert "upload_recording_chunk(buffer, audio_len)" in source
    assert "VIBE_STICK_RECORDING_AUDIO_PATH" in source
