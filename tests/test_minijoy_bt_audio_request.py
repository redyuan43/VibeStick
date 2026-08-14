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


def test_deep_sleep_gracefully_disconnects_profiles_before_power_off() -> None:
    enter_sleep = _function(
        MAIN_SOURCE,
        "static bool enter_deep_sleep(void)",
        "static void maybe_enter_deep_sleep",
    )

    assert "vibe_bt_composite_prepare_deep_sleep(uint32_t timeout_ms)" in COMPOSITE_HEADER
    assert "s_reconnect_suspended = true;" in COMPOSITE_SOURCE
    assert "esp_hf_client_disconnect(address)" in COMPOSITE_SOURCE
    assert "esp_bt_hid_device_disconnect()" in COMPOSITE_SOURCE
    assert "Bluetooth profiles disconnected for deep sleep" in COMPOSITE_SOURCE
    assert enter_sleep.index("vibe_audio_prepare_deep_sleep()") < enter_sleep.index(
        "vibe_bt_composite_prepare_deep_sleep(1200)"
    )
    assert enter_sleep.index("vibe_bt_composite_prepare_deep_sleep(1200)") < enter_sleep.index(
        "vibe_board_prepare_deep_sleep()"
    )
    assert enter_sleep.index("vibe_bt_composite_prepare_deep_sleep(1200)") < enter_sleep.index(
        "esp_deep_sleep_start()"
    )
    bluetooth_prepare = enter_sleep.split(
        "err = vibe_bt_composite_prepare_deep_sleep(1200);", 1
    )[1].split("set_minijoy_led", 1)[0]
    assert "if (err != ESP_OK)" in bluetooth_prepare
    assert "vibe_bt_composite_request_reconnect()" in bluetooth_prepare
    assert "vibe_motion_resume()" in bluetooth_prepare
    assert "return false;" in bluetooth_prepare


def test_usb_power_blocks_only_automatic_deep_sleep() -> None:
    maybe_sleep = _function(
        MAIN_SOURCE,
        "static void maybe_enter_deep_sleep(int64_t current_ms)",
        "static esp_err_t init_nvs",
    )

    assert "vibe_board_usb_powered" in maybe_sleep
    assert "vibe_minijoy_bt_should_attempt_automatic_sleep" in maybe_sleep
    assert "automatic deep sleep deferred while USB powered" in maybe_sleep
    assert "enter_deep_sleep();" in maybe_sleep

    handler = _function(MAIN_SOURCE, "static void handle_event", "static void poll_minijoy")
    forced_sleep = handler.split("case APP_EVENT_SERIAL_SLEEP:", 1)[1].split(
        "case APP_EVENT_SERIAL_REBOOT:", 1
    )[0]
    assert "vibe_board_usb_powered" not in forced_sleep


def test_serial_status_exposes_usb_power_state() -> None:
    status = _function(
        MAIN_SOURCE,
        "static void serial_status_reply(void)",
        "static uint32_t serial_pairing_seconds",
    )

    assert "vibe_board_usb_powered" in status
    assert '\\"usb_power_valid\\"' in status
    assert '\\"usb_powered\\"' in status


def test_usb_powered_idle_sends_hfp_keepalive_without_resetting_activity() -> None:
    keepalive = _function(
        MAIN_SOURCE,
        "static void maybe_send_powered_bt_keepalive(int64_t current_ms)",
        "static esp_err_t init_nvs",
    )

    assert "#define POWERED_BT_KEEPALIVE_MS 5000" in MAIN_SOURCE
    assert "vibe_board_usb_powered" in keepalive
    assert "vibe_bt_composite_keepalive" in keepalive
    assert "register_activity" not in keepalive
    assert "esp_hf_client_query_current_operator_name" not in COMPOSITE_SOURCE
    assert "esp_hidd_dev_input_set" in COMPOSITE_SOURCE
    assert "BTM_SetDefaultLinkPolicy(0)" in COMPOSITE_SOURCE
    assert "disable_sniff_policy(address)" in COMPOSITE_SOURCE
    assert "disable_sniff_policy(param->conn_stat.remote_bda)" in COMPOSITE_SOURCE
    capture_guard = keepalive.split("if (atomic_load(&s_capture_active))", 1)[1]
    capture_guard = capture_guard.split("if (current_ms < s_next_bt_keepalive_ms)", 1)[0]
    assert "s_next_bt_keepalive_ms = current_ms + POWERED_BT_KEEPALIVE_MS" in capture_guard
    assert "vibe_bt_composite_keepalive(void)" in COMPOSITE_HEADER


def test_serial_sleep_uses_the_production_deep_sleep_path() -> None:
    serial = _function(
        MAIN_SOURCE,
        "static void serial_handle_line(char *line)",
        "static void serial_maintenance_task",
    )
    handler = _function(MAIN_SOURCE, "static void handle_event", "static void poll_minijoy")
    sleep_event = handler.split("case APP_EVENT_SERIAL_SLEEP:", 1)[1].split(
        "case APP_EVENT_SERIAL_REBOOT:", 1
    )[0]

    assert 'strcasecmp(command, "SLEEP") == 0' in serial
    assert "queue_serial_event(APP_EVENT_SERIAL_SLEEP, 0);" in serial
    assert "deep_sleep_has_active_work(now_ms())" in sleep_event
    assert "enter_deep_sleep()" in sleep_event


def test_startup_reconnect_waits_for_the_host_to_finish_disconnect_cleanup() -> None:
    app_main = MAIN_SOURCE.split("void app_main(void)", 1)[1]
    bonded_startup = app_main.split("if (!initial_state.paired)", 1)[1].split(
        "update_status();", 1
    )[0]

    assert "#define RECONNECT_INITIAL_DELAY_MS 5000" in COMPOSITE_SOURCE
    assert "delayed automatic reconnect scheduled" in bonded_startup
    assert "vibe_bt_composite_request_reconnect()" not in bonded_startup


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
