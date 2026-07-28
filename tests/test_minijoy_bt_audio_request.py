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
    start = _function(MAIN_SOURCE, "static void start_ptt(void)", "static void stop_ptt(void)")
    assert start.index("vibe_audio_start()") < start.index("vibe_bt_composite_send_right_shift(true)")
    assert "vibe_bt_composite_request_audio" not in start


def test_device_ptt_keeps_pcm_alive_for_pc_release_tail() -> None:
    stop = _function(MAIN_SOURCE, "static void stop_ptt(void)", "static void start_host_capture(void)")
    assert stop.index("PTT_RELEASE_AUDIO_TAIL_MS") < stop.index("vibe_audio_stop()")
    assert "vibe_bt_composite_request_audio" not in stop
