from pathlib import Path


MAIN_SOURCE = Path("firmware/sticks3/src/main_minijoy_bt.c").read_text()
COMPOSITE_SOURCE = Path("firmware/sticks3/src/vibe_bt_composite.c").read_text()
COMPOSITE_HEADER = Path("firmware/sticks3/include/vibe_bt_composite.h").read_text()
STATUS_UI_SOURCE = Path("firmware/sticks3/src/vibe_bt_status_ui.c").read_text()


def _function(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


def test_pc_audio_gateway_owns_sco_lifecycle() -> None:
    assert "vibe_bt_composite_request_audio" not in COMPOSITE_HEADER
    assert "esp_hf_client_connect_audio" not in COMPOSITE_SOURCE
    assert "esp_hf_client_disconnect_audio" not in COMPOSITE_SOURCE


def test_connected_ui_does_not_report_missing_optional_minijoy_as_offline() -> None:
    bottom_bar = _function(
        STATUS_UI_SOURCE, "static void draw_bottom_bar(void)",
        "static bool draw_pet_frame",
    )
    assert 's_minijoy_ready ? "JOY OK"' in bottom_bar
    assert ': "BT READY"' in bottom_bar
    assert '"JOY OFF"' not in bottom_bar
    assert "s_status == VIBE_BT_UI_CONNECTED" in bottom_bar


def test_optional_imu_error_does_not_hide_working_bluetooth_profiles() -> None:
    update_status = _function(
        MAIN_SOURCE, "static void update_status(void)",
        "static void update_wake_input_guard",
    )
    assert "s_imu_error && s_air_mouse_enabled" in update_status

    status = _function(
        MAIN_SOURCE, "static void serial_status_reply(void)",
        "static uint32_t serial_pairing_seconds",
    )
    assert '\\"ui_status\\"' in status
    assert '\\"display_on\\"' in status
    assert '\\"imu_error\\"' in status


def test_hfp_connecting_wakes_before_sco_is_established() -> None:
    callback = _function(
        MAIN_SOURCE, "static void bt_state_callback(",
        "static vibe_bt_composite_state_t bt_state(void)",
    )
    assert "state->audio_connecting" in callback
    assert "APP_EVENT_HFP_AUDIO_CONNECTING" in callback

    handler = _function(
        MAIN_SOURCE, "static void handle_event",
        "static void poll_minijoy",
    )
    connecting = handler.split(
        "case APP_EVENT_HFP_AUDIO_CONNECTING:", 1
    )[1].split("case APP_EVENT_HFP_AUDIO_CONNECTED:", 1)[0]
    assert "register_activity();" in connecting
    assert "vibe_bt_composite_set_sniff_allowed(false)" in connecting
    assert "ESP_HF_CLIENT_AUDIO_STATE_CONNECTING" in COMPOSITE_SOURCE


def test_deep_sleep_gracefully_disconnects_profiles_before_power_off() -> None:
    enter_sleep = _function(
        MAIN_SOURCE,
        "static bool enter_deep_sleep(void)",
        "static void update_power_state",
    )

    assert "vibe_bt_composite_prepare_deep_sleep(uint32_t timeout_ms)" in COMPOSITE_HEADER
    assert "s_reconnect_suspended = true;" in COMPOSITE_SOURCE
    assert "esp_hf_client_disconnect(address)" in COMPOSITE_SOURCE
    assert "esp_bt_hid_device_disconnect()" in COMPOSITE_SOURCE
    assert "Bluetooth profiles disconnected for deep sleep" in COMPOSITE_SOURCE
    assert enter_sleep.index("vibe_audio_prepare_deep_sleep()") < enter_sleep.index(
        "vibe_bt_composite_prepare_deep_sleep("
    )
    assert enter_sleep.index("vibe_bt_composite_prepare_deep_sleep(") < enter_sleep.index(
        "vibe_board_prepare_deep_sleep()"
    )
    assert enter_sleep.index("vibe_bt_composite_prepare_deep_sleep(") < enter_sleep.index(
        "esp_deep_sleep_start()"
    )
    bluetooth_prepare = enter_sleep.split(
        "err = vibe_bt_composite_prepare_deep_sleep(", 1
    )[1].split("set_minijoy_led", 1)[0]
    assert "DEEP_SLEEP_BT_DISCONNECT_TIMEOUT_MS" in bluetooth_prepare
    assert "if (err != ESP_OK)" in bluetooth_prepare
    assert "vibe_bt_composite_request_reconnect()" in bluetooth_prepare
    assert "vibe_motion_resume()" in bluetooth_prepare
    assert "return false;" in bluetooth_prepare


def test_usb_power_and_power_read_failure_force_active() -> None:
    power_state = _function(
        MAIN_SOURCE,
        "static void update_power_state(int64_t current_ms)",
        "static void maybe_send_powered_bt_keepalive",
    )

    assert "vibe_board_usb_powered" in power_state
    assert "vibe_minijoy_bt_desired_power_state" in power_state
    assert "s_cached_usb_power_valid" in power_state
    assert "s_cached_usb_powered" in power_state
    assert "enter_deep_sleep();" in power_state

    handler = _function(MAIN_SOURCE, "static void handle_event", "static void poll_minijoy")
    forced_sleep = handler.split("case APP_EVENT_SERIAL_SLEEP:", 1)[1].split(
        "case APP_EVENT_SERIAL_REBOOT:", 1
    )[0]
    assert "vibe_board_usb_powered" in forced_sleep
    assert "effective_usb_power" in forced_sleep
    assert "!usb_valid || usb_powered" in forced_sleep


def test_serial_status_exposes_usb_power_state() -> None:
    status = _function(
        MAIN_SOURCE,
        "static void serial_status_reply(void)",
        "static uint32_t serial_pairing_seconds",
    )

    assert "vibe_board_usb_powered" in status
    assert '\\"usb_power_valid\\"' in status
    assert '\\"usb_powered\\"' in status


def test_three_level_power_state_and_front_wake_ptt() -> None:
    status = _function(
        MAIN_SOURCE,
        "static void serial_status_reply(void)",
        "static uint32_t serial_pairing_seconds",
    )
    assert '\\"power_state\\"' in status
    assert '\\"idle_ms\\"' in status
    assert '\\"cpu_freq_mhz\\"' in status
    assert '\\"pending_wake_ptt\\"' in status
    assert "#define STANDBY_IDLE_MS 30000" in MAIN_SOURCE
    assert "#define DEEP_SLEEP_IDLE_MS 600000" in MAIN_SOURCE
    assert "#define STANDBY_LOOP_MS 100" in MAIN_SOURCE
    assert ".min_freq_mhz = 80" in MAIN_SOURCE
    assert ".light_sleep_enable = false" in MAIN_SOURCE
    assert "ESP_PM_CPU_FREQ_MAX" in MAIN_SOURCE
    assert "vibe_bt_status_ui_enter_standby" in MAIN_SOURCE

    app_main = MAIN_SOURCE.split("void app_main(void)", 1)[1]
    assert "woke_from_front" in app_main
    assert "s_pending_wake_ptt = true" in app_main
    assert "update_pending_wake_ptt(current_ms)" in app_main

    wake_ptt = _function(
        MAIN_SOURCE,
        "static void update_pending_wake_ptt(int64_t current_ms)",
        "static void poll_front_button",
    )
    assert "atomic_load(&s_wake_input_guard)" in wake_ptt
    assert "state.hid_connected" in wake_ptt
    assert "state.hfp_connected" in wake_ptt
    assert "start_ptt();" in wake_ptt
    assert "WAKE_PTT_TIMEOUT_MS" in wake_ptt


def test_wake_ptt_hands_capture_to_host_and_cannot_stick_forever() -> None:
    handler = _function(
        MAIN_SOURCE, "static void handle_event",
        "static void poll_minijoy",
    )
    audio_connected = handler.split(
        "case APP_EVENT_HFP_AUDIO_CONNECTED:", 1
    )[1].split("case APP_EVENT_HFP_AUDIO_DISCONNECTED:", 1)[0]
    assert "handoff_wake_ptt_to_host_capture()" in audio_connected

    handoff = _function(
        MAIN_SOURCE, "static bool handoff_wake_ptt_to_host_capture(void)",
        "static void start_host_capture(void)",
    )
    assert "vibe_bt_composite_send_right_shift(false)" in handoff
    assert "s_capture_owner = CAPTURE_OWNER_HOST_HFP" in handoff
    assert "vibe_minijoy_ptt_audio_clear" in handoff

    wake_ptt = _function(
        MAIN_SOURCE, "static void update_pending_wake_ptt(int64_t current_ms)",
        "static void poll_front_button",
    )
    assert "WAKE_PTT_HANDOFF_TIMEOUT_MS" in wake_ptt
    assert "stop_ptt(false)" in wake_ptt
    assert "s_wake_ptt_handoff_pending = true" in wake_ptt


def test_volatile_power_test_environment_can_exercise_deep_wake() -> None:
    serial = _function(
        MAIN_SOURCE,
        "static void serial_handle_line(char *line)",
        "static void serial_maintenance_task",
    )
    assert 'strcasecmp(command, "TEST_POWER") == 0' in serial
    assert 'strcasecmp(argument, "BATTERY_FAST") == 0' in serial
    assert "APP_EVENT_SERIAL_TEST_POWER" in serial
    assert "#define POWER_TEST_STANDBY_MS 5000" in MAIN_SOURCE
    assert "#define POWER_TEST_BATTERY_DEEP_SLEEP_MS 120000" in MAIN_SOURCE
    assert "#define POWER_TEST_DEEP_SLEEP_MS 20000" in MAIN_SOURCE
    assert "mode == POWER_TEST_BATTERY" in MAIN_SOURCE
    assert "return POWER_TEST_BATTERY_DEEP_SLEEP_MS;" in MAIN_SOURCE
    assert '"entering deep sleep idle_ms=%lld' in MAIN_SOURCE
    assert "power_deep_sleep_timeout_ms(s_power_test_mode)" in MAIN_SOURCE
    assert "RTC_DATA_ATTR uint32_t s_power_test_wake_magic" in MAIN_SOURCE
    assert "esp_sleep_enable_timer_wakeup(POWER_TEST_TIMER_WAKE_US)" in MAIN_SOURCE
    assert "diagnostic timer wake injected as front-button wake" in MAIN_SOURCE
    assert '\\"power_test_mode\\"' in MAIN_SOURCE


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
    assert "vibe_bt_composite_set_sniff_allowed(false)" in COMPOSITE_SOURCE
    assert "apply_sniff_policy(param->conn_stat.remote_bda)" in COMPOSITE_SOURCE
    capture_guard = keepalive.split("if (atomic_load(&s_capture_active))", 1)[1]
    capture_guard = capture_guard.split("if (current_ms < s_next_bt_keepalive_ms)", 1)[0]
    assert "s_next_bt_keepalive_ms = current_ms + POWERED_BT_KEEPALIVE_MS" in capture_guard
    assert "vibe_bt_composite_keepalive(void)" in COMPOSITE_HEADER


def test_battery_standby_keeps_classic_acl_active_and_wakes_before_audio() -> None:
    power_state_transition = _function(
        MAIN_SOURCE,
        "static void set_power_state(vibe_minijoy_power_state_t state)",
        "static int64_t now_ms(void)",
    )
    assert "vibe_bt_composite_set_sniff_allowed" not in power_state_transition
    standby_transition = power_state_transition.split(
        "state == VIBE_MINIJOY_POWER_STANDBY", 1
    )[1]
    assert "configure_cpu_ceiling(240);" in standby_transition
    assert "esp_pm_lock_release" not in standby_transition
    assert "configure_cpu_ceiling(80);" not in standby_transition
    power_state = _function(
        MAIN_SOURCE,
        "static void update_power_state(int64_t current_ms)",
        "static void maybe_send_powered_bt_keepalive",
    )
    assert "vibe_bt_composite_set_sniff_allowed(false)" in power_state
    assert "vibe_bt_composite_set_sniff_allowed(true)" not in power_state
    assert "vibe_bt_composite_set_sniff_allowed(bool allowed)" in COMPOSITE_SOURCE
    assert "CLASSIC_LINK_POLICY_SNIFF" in COMPOSITE_SOURCE

    host_capture = _function(
        MAIN_SOURCE, "static void start_host_capture(void)",
        "static void stop_host_capture(void)",
    )
    assert host_capture.index("register_activity();") < host_capture.index(
        "vibe_audio_start();"
    )


def test_missing_minijoy_uses_bounded_exponential_retry() -> None:
    open_minijoy = _function(
        MAIN_SOURCE, "static void open_minijoy(void)",
        "static void start_ptt(void)",
    )
    assert "#define JOY_RETRY_MAX_MS 30000" in MAIN_SOURCE
    assert "s_minijoy_retry_delay_ms" in open_minijoy
    assert "retry_ms * 2" in open_minijoy
    assert "? JOY_RETRY_MAX_MS" in open_minijoy
    assert "s_minijoy_retry_delay_ms = JOY_RETRY_MS" in open_minijoy


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
