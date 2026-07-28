from pathlib import Path


MAIN_SOURCE = Path("firmware/sticks3/src/main_minijoy_bt.c").read_text()
COMPOSITE_SOURCE = Path("firmware/sticks3/src/vibe_bt_composite.c").read_text()
COMPOSITE_HEADER = Path("firmware/sticks3/include/vibe_bt_composite.h").read_text()


def _function(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


def test_pc_audio_gateway_owns_sco_lifecycle() -> None:
    assert "vibe_bt_composite_request_audio" not in COMPOSITE_HEADER
    assert "esp_hf_client_connect_audio" not in COMPOSITE_SOURCE
    assert "esp_hf_client_disconnect_audio" not in COMPOSITE_SOURCE


def test_device_ptt_starts_pcm_then_notifies_pc_with_right_shift() -> None:
    start = _function(MAIN_SOURCE, "static void start_ptt(void)", "static void stop_ptt(bool arm_followup)")
    assert start.index("vibe_audio_start()") < start.index("vibe_bt_composite_send_right_shift(true)")
    assert "vibe_bt_composite_request_audio" not in start


def test_device_ptt_keeps_pcm_alive_for_pc_release_tail() -> None:
    stop = _function(MAIN_SOURCE, "static void stop_ptt(bool arm_followup)", "static void start_host_capture(void)")
    assert stop.index("PTT_RELEASE_AUDIO_TAIL_MS") < stop.index("vibe_audio_stop()")
    assert "vibe_bt_composite_request_audio" not in stop


def test_device_ptt_reports_missing_sco_after_grace_and_recovers() -> None:
    start = _function(MAIN_SOURCE, "static void start_ptt(void)", "static void stop_ptt(bool arm_followup)")
    stop = _function(MAIN_SOURCE, "static void stop_ptt(bool arm_followup)", "static void start_host_capture(void)")
    status = _function(MAIN_SOURCE, "static void update_status(void)", "static void update_wake_input_guard")

    assert "#define PTT_AUDIO_CONNECT_GRACE_MS 3000" in MAIN_SOURCE
    assert "vibe_minijoy_ptt_audio_begin" in start
    assert "vibe_minijoy_ptt_audio_clear" in stop
    assert "vibe_minijoy_ptt_audio_update" in status
    assert "VIBE_MINIJOY_PTT_AUDIO_FAILED" in status
    assert "VIBE_BT_UI_AUDIO_FAILED" in status


def test_serial_ptt_never_uses_physical_followup_handler() -> None:
    handler = _function(MAIN_SOURCE, "static void handle_event", "static void poll_minijoy")
    serial = handler.split("case APP_EVENT_SERIAL_PTT_DOWN:", 1)[1].split(
        "case APP_EVENT_SERIAL_REBOOT:", 1
    )[0]

    assert "vibe_minijoy_ptt_press_action(" in serial
    assert "start_ptt();" in serial
    assert "stop_ptt(false);" in serial
    assert "handle_front_down" not in serial
    assert "handle_front_up" not in serial
