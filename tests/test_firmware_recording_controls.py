from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = ROOT / "firmware" / "sticks3" / "src" / "main.c"
BOARD_PROFILE_H = ROOT / "firmware" / "sticks3" / "include" / "vibe_board_profile.h"


def test_front_single_click_toggles_device_recording() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_EVENT_RECORDING_TOGGLE" in source
    assert "queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);" in source
    assert 'handle_recording_start("button_tap_start", "再按发送")' in source
    assert 'handle_recording_stop("button_tap_stop")' in source
    assert 'post_simple_event("button_short", NULL)' not in source


def test_tap_recording_uses_existing_external_pcm_upload_path() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "start_recording_upload_task()" in source
    assert "upload_recording_chunk(buffer, audio_len)" in source
    assert "VIBE_STICK_RECORDING_AUDIO_PATH" in source


def test_recording_upload_keeps_append_chunks_and_logs_diagnostics() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "append=1" in source
    assert "recording diagnostics board=%s" in source
    assert "esp_wifi_sta_get_ap_info" in source
    assert "post_ms_min" in source


def test_idle_backlight_has_dim_and_off_states() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    board_profile = BOARD_PROFILE_H.read_text(encoding="utf-8")

    assert "VIBE_STICK_IDLE_DIM_MS 30000" in source
    assert "VIBE_STICK_IDLE_OFF_MS 60000" in source
    assert "VIBE_STICK_BACKLIGHT_FADE_INTERVAL_MS" in source
    assert "fade_backlight_toward(target, now_ms)" in source
    assert "DISPLAY_POWER_DIMMED" in source
    assert "DISPLAY_POWER_OFF" in source
    assert "#define VIBE_BOARD_LCD_BACKLIGHT_IDLE 45" in board_profile


def test_lift_motion_start_is_deferred_instead_of_dropped() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_motion_start_pending" in source
    assert "request_motion_recording_start()" in source
    assert "motion lift start deferred while recording network is busy" in source
    assert "motion lift start deferred request cancelled by flat posture" in source
    assert "queue_event(VIBE_STICK_EVENT_MOTION_START)" in source


def test_deep_sleep_keeps_button_wake_and_guards_lift_mode() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    board_profile = BOARD_PROFILE_H.read_text(encoding="utf-8")

    assert "VIBE_STICK_DEEP_SLEEP_MS VIBE_STICK_IDLE_OFF_MS" in source
    assert "maybe_enter_deep_sleep(now_ms)" in source
    assert "esp_deep_sleep_start()" in source
    assert "esp_sleep_enable_ext0_wakeup(ext0_gpio, 0)" in source
    assert "esp_sleep_enable_ext1_wakeup_io(wake_mask" in source
    assert "vibe_motion_prepare_deep_sleep_wake()" in source
    assert "#define VIBE_BOARD_PIN_IMU_INT GPIO_NUM_35" in board_profile
    assert "#define VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE 0" in board_profile


def test_wifi_profiles_are_persisted_and_rotated() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "WIFI_PROFILE_NAMESPACE" in source
    assert "wifi_profiles_load_nvs" in source
    assert "wifi_profiles_save_nvs" in source
    assert "wifi_profiles_merge_configured" in source
    assert "VIBE_STICK_WIFI_PROFILES" in source
    assert "s_wifi_profile_index = (s_wifi_profile_index + 1) % s_wifi_profile_count" in source
