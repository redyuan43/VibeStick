from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = ROOT / "firmware" / "sticks3" / "src" / "main.c"
AUDIO_C = ROOT / "firmware" / "sticks3" / "src" / "vibe_audio.c"
BOARD_PROFILE_H = ROOT / "firmware" / "sticks3" / "include" / "vibe_board_profile.h"


def test_front_single_click_toggles_device_recording() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_EVENT_RECORDING_TOGGLE" in source
    assert "queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);" in source
    assert 'handle_recording_start("button_tap_start", "再按发送")' in source
    assert 'handle_recording_stop("button_tap_stop")' in source
    assert 'post_simple_event("button_short", NULL)' not in source


def test_ptt_release_followup_short_click_sends_enter_before_tap_toggle() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_single_click = source.split("static void button_single_click_cb", 1)[1]
    button_single_click = button_single_click.split("static void button_double_click_cb", 1)[0]

    assert "#define PTT_ENTER_GRACE_MS 5000" in source
    assert "VIBE_STICK_EVENT_PTT_FOLLOWUP_ENTER" in source
    assert '\\"event\\":\\"%s\\"' in source
    assert '"button_followup_enter"' in source
    assert '\\"session_id\\":\\"%s\\"' in source
    assert button_single_click.index("consume_ptt_followup_enter_window()") < button_single_click.index(
        "queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE)"
    )


def test_ptt_release_followup_double_click_sends_escape_before_double_click_action() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_double_click = source.split("static void button_double_click_cb", 1)[1]
    button_double_click = button_double_click.split("static void side_button_long_start_cb", 1)[0]

    assert "VIBE_STICK_EVENT_PTT_FOLLOWUP_ESCAPE" in source
    assert '\\"event\\":\\"%s\\"' in source
    assert '"button_followup_escape"' in source
    assert button_double_click.index("consume_ptt_followup_enter_window()") < button_double_click.index(
        "queue_event(VIBE_STICK_EVENT_DOUBLE_CLICK)"
    )


def test_ptt_followup_enter_arms_after_long_press_or_tap_stop() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    handle_stop = source.split("static void handle_recording_stop", 1)[1]
    handle_stop = handle_stop.split("static void handle_recording_toggle", 1)[0]
    handle_toggle = source.split("static void handle_recording_toggle", 1)[1]
    handle_toggle = handle_toggle.split("static bool wifi_profile_has_ssid", 1)[0]

    assert 'strcmp(event_name, "button_long_stop") == 0' in handle_stop
    assert 'strcmp(event_name, "button_tap_stop") == 0' in handle_stop
    assert "arm_ptt_followup_enter_window();" in handle_stop
    assert "clear_ptt_followup_enter_window();" in handle_stop
    assert 'handle_recording_stop("button_tap_stop")' in handle_toggle
    assert 'handle_recording_start("button_tap_start", "再按发送")' in handle_toggle


def test_tap_recording_uses_existing_external_pcm_upload_path() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "start_recording_upload_task()" in source
    assert "upload_recording_chunk(buffer, audio_len)" in source
    assert "VIBE_STICK_RECORDING_AUDIO_PATH" in source


def test_recording_start_uses_descending_chirp_and_stop_uses_legacy_beep() -> None:
    source = AUDIO_C.read_text(encoding="utf-8")
    recording_start = source.split("static const sound_segment_t recording_start[] = {", 1)[1]
    recording_start = recording_start.split("};", 1)[0]
    recording_stop = source.split("static const sound_segment_t recording_stop[] = {", 1)[1]
    recording_stop = recording_stop.split("};", 1)[0]

    assert "VIBE_STICK_RECORDING_CHIRP_MS" in source
    assert "VIBE_STICK_RECORDING_CHIRP_GAP_MS" in source
    assert "{.freq_hz = 3600, .duration_ms = VIBE_STICK_RECORDING_CHIRP_MS}" in recording_start
    assert "{.freq_hz = 1800, .duration_ms = VIBE_STICK_RECORDING_CHIRP_MS}" in recording_start
    assert "{.freq_hz = 4000, .duration_ms = VIBE_STICK_BEEP_MS}" in recording_stop
    assert "VIBE_STICK_SOUND_RECORDING_START" in source
    assert "VIBE_STICK_SOUND_RECORDING_STOP" in source


def test_followup_enter_and_escape_use_distinct_buzz_sounds() -> None:
    source = AUDIO_C.read_text(encoding="utf-8")
    header = (ROOT / "firmware" / "sticks3" / "include" / "vibe_audio.h").read_text(
        encoding="utf-8"
    )
    main_source = MAIN_C.read_text(encoding="utf-8")
    followup_enter = source.split("static const sound_segment_t followup_enter[] = {", 1)[1]
    followup_enter = followup_enter.split("};", 1)[0]
    followup_escape = source.split("static const sound_segment_t followup_escape[] = {", 1)[1]
    followup_escape = followup_escape.split("};", 1)[0]

    assert "VIBE_STICK_SOUND_FOLLOWUP_ENTER" in header
    assert "VIBE_STICK_SOUND_FOLLOWUP_ESCAPE" in header
    assert "VIBE_STICK_FOLLOWUP_BUZZ_MS" in source
    assert "VIBE_STICK_FOLLOWUP_BUZZ_GAP_MS" in source
    assert "{.freq_hz = 2600, .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS}" in followup_enter
    assert "{.freq_hz = 3200, .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS}" in followup_enter
    assert "{.freq_hz = 2100, .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS}" in followup_escape
    assert "{.freq_hz = 1200, .duration_ms = VIBE_STICK_FOLLOWUP_BUZZ_MS}" in followup_escape
    assert followup_enter != followup_escape
    assert "post_ptt_followup_key_event(\"button_followup_enter\"" in main_source
    assert "post_ptt_followup_key_event(\"button_followup_escape\"" in main_source


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
    assert "VIBE_STICK_IDLE_STATE_POLL_MS" not in source
    assert "VIBE_STICK_BACKLIGHT_FADE_INTERVAL_MS" in source
    assert "fade_backlight_toward(target, now_ms)" in source
    assert "DISPLAY_POWER_DIMMED" in source
    assert "DISPLAY_POWER_OFF" in source
    assert "#define VIBE_BOARD_LCD_BACKLIGHT_IDLE 45" in board_profile


def test_usb_power_keeps_display_active() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "static bool external_powered(void)" in source
    assert "return s_state.battery_charging || s_state.usb_powered;" in source
    assert "return external_powered() ||" in source
    assert "deep_sleep_should_stay_awake() ||" in source


def test_state_polling_stops_while_screen_is_off() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_display_power_state == DISPLAY_POWER_ACTIVE" in source
    assert "now_ms - last_poll >= VIBE_STICK_STATE_POLL_MS" in source
    assert "VIBE_STICK_IDLE_STATE_POLL_MS" not in source


def test_ota_check_blocks_sleep_without_waking_display() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    display_guard = source.split("static bool display_should_stay_active(void)", 1)[1]
    display_guard = display_guard.split("static bool deep_sleep_should_stay_awake(void)", 1)[0]

    assert "ota_in_progress()" not in display_guard
    assert "static bool deep_sleep_should_stay_awake(void)" in source
    assert "return display_should_stay_active() || ota_in_progress();" in source
    assert "deep_sleep_should_stay_awake() ||" in source


def test_ota_check_runs_on_network_wake_without_periodic_polling() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    config = (ROOT / "firmware" / "sticks3" / "include" / "vibe_stick_config.h").read_text(
        encoding="utf-8"
    )

    assert "queue_event(VIBE_STICK_EVENT_OTA_CHECK);" in source
    assert "case VIBE_STICK_EVENT_OTA_CHECK:" in source
    assert "start_ota_check_task();" in source
    assert "s_last_ota_check_ms" not in source
    assert "VIBE_STICK_OTA_CHECK_MS" not in source
    assert "VIBE_STICK_OTA_CHECK_MS" not in config


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

    assert "VIBE_STICK_DEEP_SLEEP_MS 600000" in source
    assert "maybe_enter_deep_sleep(now_ms)" in source
    assert "esp_deep_sleep_start()" in source
    assert "esp_sleep_enable_ext0_wakeup(ext0_gpio, 0)" in source
    assert "esp_sleep_enable_ext1_wakeup_io(wake_mask" in source
    assert "vibe_motion_prepare_deep_sleep_wake()" in source
    assert "#define VIBE_BOARD_PIN_IMU_INT GPIO_NUM_35" in board_profile
    assert "#define VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE 0" in board_profile


def test_recording_mode_preference_survives_deep_sleep_restart() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert 'DEVICE_PREF_NAMESPACE "vibe_prefs"' in source
    assert 'DEVICE_PREF_RECORDING_MODE_KEY "rec_mode"' in source
    assert "save_recording_mode_preference(s_recording_mode)" in source
    assert "restore_recording_mode_preference()" in source
    assert "nvs_get_u8(handle, DEVICE_PREF_RECORDING_MODE_KEY" in source
    assert "vibe_motion_recalibrate()" in source


def test_deep_sleep_button_wake_restores_ptt_hold() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_woke_from_deep_sleep = wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED;" in source
    assert "capture_deep_sleep_front_button_intent()" in source
    assert "handle_deep_sleep_front_button_intent()" in source
    assert "front button held during deep sleep wake; pending PTT restore" in source
    assert "restoring front long press after deep sleep wake" in source
    assert "s_wake_front_button_pending = false;" in source
    assert 'handle_recording_start("button_long_start", "松开发送")' in source


def test_deep_sleep_wake_defers_pet_animation_until_state_restores() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_PET_FAST_RESUME_MAX_MS 15000" in source
    assert "s_pet_fast_resume_pending = true;" in source
    assert "static void complete_pet_fast_resume(void)" in source
    assert "if (s_pet_fast_resume_pending)" in source
    assert "complete_pet_fast_resume();" in source
    assert "VIBE_STICK_DEEP_SLEEP_FAST_RESUME_MS" not in source


def test_wifi_start_overlaps_display_setup_during_wake() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    app_main = source.split("void app_main(void)", 1)[1]

    assert "static bool s_ui_ready;" in source
    assert "if (!s_ui_ready)" in source
    assert app_main.index("ESP_ERROR_CHECK(init_wifi());") < app_main.index(
        "ESP_ERROR_CHECK(init_display());"
    )
    assert app_main.index("s_ui_ready = true;") < app_main.index("render_state();")


def test_wifi_profiles_are_persisted_and_rotated() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "WIFI_PROFILE_NAMESPACE" in source
    assert "wifi_profiles_load_nvs" in source
    assert "wifi_profiles_save_nvs" in source
    assert "wifi_profiles_merge_configured" in source
    assert "VIBE_STICK_WIFI_PROFILES" in source
    assert "s_wifi_profile_index = (s_wifi_profile_index + 1) % s_wifi_profile_count" in source
