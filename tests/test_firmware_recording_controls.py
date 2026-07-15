import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = ROOT / "firmware" / "sticks3" / "src" / "main.c"
AUDIO_C = ROOT / "firmware" / "sticks3" / "src" / "vibe_audio.c"
BOARD_C = ROOT / "firmware" / "sticks3" / "src" / "vibe_board.c"
BOARD_H = ROOT / "firmware" / "sticks3" / "include" / "vibe_board.h"
BOARD_PROFILE_H = ROOT / "firmware" / "sticks3" / "include" / "vibe_board_profile.h"


def test_front_single_click_toggles_device_recording() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_EVENT_RECORDING_TOGGLE" in source
    assert "queue_event(VIBE_STICK_EVENT_RECORDING_TOGGLE);" in source
    assert 'handle_recording_start("button_tap_start", "TAP TO SEND")' in source
    assert 'handle_recording_stop("button_tap_stop")' in source
    assert "front tap toggle mode=%s" in source
    assert "front button down mode=%s" in source
    assert "front single click mode=%s" in source
    assert "front gpio fallback down mode=%s" in source
    assert "front gpio fallback single duration=%lld mode=%s" in source
    assert 'post_simple_event("button_short", NULL)' not in source


def test_cyber_front_gpio_fallback_does_not_duplicate_iot_button_events() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    fallback = source.split("static void poll_front_button_fallback", 1)[1]
    fallback = fallback.split("static void register_activity", 1)[0]
    button_up = source.split("static void button_up_cb", 1)[1]
    button_up = button_up.split("static esp_err_t init_button", 1)[0]

    assert "static bool front_button_iot_handled_press" in source
    assert "s_front_button_iot_up_ms" in source
    assert "s_front_fallback_suppressed = front_button_iot_handled_press(now_ms);" in fallback
    assert "front_button_iot_handled_press(now_ms)" in fallback
    assert "now_ms - s_front_button_iot_single_ms < 250" in fallback
    assert "now_ms - s_front_button_iot_up_ms < 250" in fallback
    assert "s_front_button_iot_up_ms = now_ms;" in button_up


def test_ptt_release_followup_short_click_sends_enter_before_tap_toggle() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_single_click = source.split("static void button_single_click_cb", 1)[1]
    button_single_click = button_single_click.split("static void button_double_click_cb", 1)[0]

    assert "#define PTT_ENTER_GRACE_MS 3000" in source
    assert "#define PTT_FOLLOWUP_REQUEST_TIMEOUT_MS 1000" in source
    assert '\\"event\\":\\"%s\\"' in source
    assert '"button_followup_enter"' in source
    assert '\\"session_id\\":\\"%s\\"' in source
    assert "static void ptt_followup_key_dispatch_task" in source
    assert "start_ptt_followup_key_dispatch" in source
    assert "VIBE_STICK_FOLLOWUP_CORE VIBE_STICK_APP_CORE" in source
    assert "VIBE_STICK_FOLLOWUP_PRIORITY 6" in source
    assert "PTT_FOLLOWUP_REQUEST_TIMEOUT_MS" in source
    assert "free(dispatch);" in source
    assert "ptt_followup_enter_window_present()" in button_single_click
    assert "recording_finalize_active()" in button_single_click
    assert "front single click ignored after dictation stop" in button_single_click
    assert "recording_intent_is_cyber()" in button_single_click
    assert button_single_click.index("recording_intent_is_cyber()") < button_single_click.index(
        "consume_ptt_followup_enter_window()"
    )
    assert button_single_click.index("consume_ptt_followup_enter_window()") < button_single_click.index(
        "start_ptt_followup_key_dispatch"
    )
    assert "queue_event(VIBE_STICK_EVENT_PTT_FOLLOWUP_ENTER)" not in button_single_click


def test_ptt_release_followup_double_click_requests_current_dictation_cancellation() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_double_click = source.split("static void button_double_click_cb", 1)[1]
    button_double_click = button_double_click.split("static void side_button_long_start_cb", 1)[0]

    assert '\\"event\\":\\"%s\\"' in source
    assert '"button_followup_escape"' in source
    assert button_double_click.index("consume_ptt_followup_enter_window()") < button_double_click.index(
        "start_ptt_followup_key_dispatch"
    )
    assert "ptt_followup_enter_window_present()" in button_double_click
    assert "recording_finalize_active()" in button_double_click
    assert "front double click ignored after dictation stop" in button_double_click
    assert "queue_event(VIBE_STICK_EVENT_PTT_FOLLOWUP_ESCAPE)" not in button_double_click


def test_ptt_followup_accepts_a_deliberate_press_without_starting_another_recording() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_long = source.split("static void button_long_start_cb", 1)[1]
    button_long = button_long.split("static void bridge_selection_confirm_long_cb", 1)[0]
    init_button = source.split("static esp_err_t init_button", 1)[1]
    init_button = init_button.split("static void restore_wake_button_intent", 1)[0]

    assert "#define FRONT_PTT_LONG_PRESS_MS 400" in source
    assert ".press_time = FRONT_PTT_LONG_PRESS_MS" in init_button
    assert "ptt_followup_enter_window_present()" in button_long
    assert "consume_ptt_followup_enter_window()" in button_long
    assert '"button_followup_enter"' in button_long
    assert "front long press accepted as dictation confirmation" in button_long
    assert button_long.index("consume_ptt_followup_enter_window()") < button_long.index(
        "s_tap_recording_active"
    )


def test_ptt_followup_enter_arms_after_long_press_or_tap_stop() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    handle_stop = source.split("static void handle_recording_stop", 1)[1]
    handle_stop = handle_stop.split("static void handle_recording_toggle", 1)[0]
    handle_toggle = source.split("static void handle_recording_toggle", 1)[1]
    handle_toggle = handle_toggle.split("static bool wifi_profile_has_ssid", 1)[0]

    assert 'strcmp(event_name, "button_long_stop") == 0' in handle_stop
    assert 'strcmp(event_name, "button_tap_stop") == 0' in handle_stop
    assert "!recording_intent_is_cyber()" in handle_stop
    assert "arm_ptt_followup_enter_window();" in handle_stop
    assert "clear_ptt_followup_enter_window();" in handle_stop
    finish_stop = source.split("static void finish_recording_stop", 1)[1].split(
        "static void recording_finalize_task", 1
    )[0]
    assert "clear_ptt_followup_enter_window();" not in finish_stop
    assert 'handle_recording_stop("button_tap_stop")' in handle_toggle
    assert 'handle_recording_start("button_tap_start", "TAP TO SEND")' in handle_toggle


def test_tap_recording_uses_existing_external_pcm_upload_path() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "start_recording_upload_task()" in source
    assert "upload_recording_chunk(buffer, audio_len)" in source
    assert "VIBE_STICK_RECORDING_AUDIO_PATH" in source
    assert '\\"session_id\\":\\"%s\\",\\"intent\\":\\"%s\\",\\"mode\\":\\"%s\\"' in source


def test_side_button_discovers_and_persists_multiple_lan_bridges() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    bridge_probe = source.split("static bool bridge_probe_discovered", 1)[1]
    bridge_probe = bridge_probe.split("static bool bridge_probe_profile", 1)[0]
    socket_wait = source.split("static void bridge_wait_for_socket_connections", 1)[1]
    socket_wait = socket_wait.split("static size_t bridge_discover_subnet_profiles", 1)[0]
    discovery = source.split("static size_t bridge_discover_subnet_profiles", 1)[1]
    discovery = discovery.split("static bool bridge_discovered_profile_equal", 1)[0]
    merge = source.split("static bool bridge_profiles_merge_scan_results", 1)[1]
    merge = merge.split("static void bridge_discovery_task", 1)[0]
    task = source.split("static void bridge_discovery_task", 1)[1]
    task = task.split("static bool start_bridge_discovery_task", 1)[0]
    start_task = source.split("static bool start_bridge_discovery_task", 1)[1]
    start_task = start_task.split("static void bridge_ensure_target", 1)[0]
    bridge_load = source.split("static esp_err_t bridge_target_load_nvs", 1)[1]
    bridge_load = bridge_load.split("static esp_err_t bridge_target_save_nvs", 1)[0]

    assert 'http_request_target("GET", host, port, "", "/health"' in bridge_probe
    assert "k_configured_bridge_profiles[index].token" in bridge_probe
    assert "bridge_parse_discovered_health" in bridge_probe
    assert "BRIDGE_DISCOVERY_SOCKET_BATCH_SIZE" in discovery
    assert "bridge_wait_for_socket_connections(" in discovery
    assert "select(max_socket + 1, NULL, &write_fds" in socket_wait
    assert "while (true)" in socket_wait
    assert "deadline_us" in socket_wait
    assert "settled[index] = true" in socket_wait
    assert "next_host_id = 254" in discovery
    assert "recording_network_busy()" in discovery
    assert "bridge_profiles_merge_scan_results" not in discovery
    assert "bridge_profiles_save_nvs" not in discovery
    assert "s_bridge_scan_profiles[s_bridge_scan_profile_count++] = *profile" in source
    assert "for (size_t scan_index = 0;" in merge
    assert "bridge_profiles_save_nvs(scan_ssid)" in merge
    assert "changed" in merge
    assert task.count("bridge_profiles_merge_scan_results(scan_ssid)") == 1
    assert "bridge_discover_subnet_profiles()" in task
    assert "bridge_target_set_profile" not in task
    assert '"manual-search"' not in task
    assert "show_persistent_mode_switch_visual" in start_task
    assert "BRIDGE_PROFILE_STORE_KEY" in source
    assert "nvs_get_blob(handle, BRIDGE_PROFILE_STORE_KEY" in source
    assert "nvs_set_blob(handle, BRIDGE_PROFILE_STORE_KEY" in source
    assert "bridge_profile_index_by_id(profile_id)" in bridge_load


def test_bridge_discovery_fallback_id_is_bounded() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    fallback = source.split("static void bridge_discovery_fallback_id", 1)[1]
    fallback = fallback.split("static bool bridge_parse_discovered_health", 1)[0]

    assert 'strlcpy(id, "lan-", id_len);' in fallback
    assert 'strlcat(id, host && host[0] != \'\\0\' ? host : "bridge", id_len);' in fallback
    assert "snprintf(id, id_len" not in fallback


def test_battery_curves_are_board_specific_and_calibrated() -> None:
    board = BOARD_C.read_text(encoding="utf-8")
    profile = BOARD_PROFILE_H.read_text(encoding="utf-8")

    assert '#include "sticks3_battery_curve.h"' in board
    assert "*level_percent = vibe_sticks3_battery_percent(voltage_mv);" in board
    assert "{3082, 0}" in board
    assert "{3625, 50}" in board
    assert "{4042, 100}" in board
    assert 'VIBE_BOARD_BATTERY_CURVE_VERSION "sticks3-20260714-full-v1"' in profile
    assert 'VIBE_BOARD_BATTERY_CURVE_VERSION "stickc-plus-20260714-full-v1"' in profile


def test_idle_pet_bobs_briefly_then_uses_a_low_frequency_static_timer() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    activity = source.split("static void register_activity(void)\n{", 1)[1]
    activity = activity.split("static void update_power_saving", 1)[0]
    update_pet = source.split("static void update_pet_visual(void)\n{", 1)[1]
    update_pet = update_pet.split("static void pet_timer_cb", 1)[0]

    assert "#define VIBE_STICK_PET_ACTIVE_TIMER_MS 300" in source
    assert "#define VIBE_STICK_PET_IDLE_TIMER_MS 1000" in source
    assert "#define VIBE_STICK_PET_IDLE_BOB_STEPS 16" in source
    assert "static int s_pet_idle_bob_steps_remaining;" in source
    assert "s_pet_idle_bob_steps_remaining = VIBE_STICK_PET_IDLE_BOB_STEPS;" in activity
    assert "lv_timer_set_period(s_pet_timer, VIBE_STICK_PET_ACTIVE_TIMER_MS);" in activity
    assert "sequence.key != 0 || s_pet_idle_bob_steps_remaining > 0" in update_pet
    assert "s_pet_idle_bob_steps_remaining--;" in update_pet
    assert "set_pet_timer_period(VIBE_STICK_PET_IDLE_TIMER_MS);" in update_pet
    assert "set_pet_vertical_offset(14);" in update_pet


def test_side_button_only_starts_full_scan_and_arms_selection_window() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    side_up = source.split("static void side_button_up_cb", 1)[1]
    side_up = side_up.split("static void button_long_start_cb", 1)[0]
    start_task = source.split("static bool start_bridge_discovery_task", 1)[1]
    start_task = start_task.split("static void bridge_ensure_target", 1)[0]
    cycle = source.split("static void cycle_bridge_profile(void)\n{", 1)[1]
    cycle = cycle.split("static esp_err_t bridge_prepare_active_target", 1)[0]

    assert "static atomic_bool s_bridge_discovery_active;" in source
    assert "static TaskHandle_t s_bridge_discovery_task;" in source
    assert "atomic_compare_exchange_strong(&s_bridge_discovery_active" in start_task
    assert "bridge discovery already running" in start_task
    assert "xTaskCreatePinnedToCore(bridge_discovery_task" in start_task
    assert "#define BRIDGE_SELECTION_ENTRY_WINDOW_MS 5000" in source
    assert "VIBE_STICK_EVENT_BRIDGE_SCAN_FULL" in side_up
    assert "BRIDGE_SELECTION_ENTRY_WINDOW_MS" in side_up
    assert "VIBE_STICK_EVENT_BRIDGE_SELECTION_NEXT" not in side_up
    assert "start_bridge_discovery_task" not in cycle
    assert "bridge_saved_profile_count()" in cycle
    assert "bridge_target_set_profile(next_index, \"manual\", false)" in cycle
    assert 'show_bridge_selection_visual("CONNECTING"' in cycle
    assert "queue_event(VIBE_STICK_EVENT_POLL_STATE)" in cycle
    assert "bridge_profiles_reachable_ordered" not in cycle
    assert "bridge_probe_profile(" not in cycle


def test_front_button_enters_persistent_bridge_selection_and_confirms_on_hold() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    button_down = source.split("static void button_press_down_cb", 1)[1]
    button_down = button_down.split("static void button_single_click_cb", 1)[0]
    button_up = source.split("static void button_up_cb", 1)[1]
    button_up = button_up.split("static esp_err_t init_button", 1)[0]
    confirm = source.split("static void bridge_selection_confirm_long_cb", 1)[1]
    confirm = confirm.split("static void button_up_cb", 1)[0]
    init_button = source.split("static esp_err_t init_button", 1)[1]
    init_button = init_button.split("static void restore_wake_button_intent", 1)[0]

    assert "static atomic_bool s_bridge_selection_active;" in source
    assert "static atomic_int_fast64_t s_bridge_selection_entry_deadline_ms;" in source
    assert "entry_deadline > 0 && now_ms <= entry_deadline" in button_down
    assert "bridge selection mode entered" in button_down
    assert "queue_bridge_control(BRIDGE_CONTROL_NEXT)" in button_up
    assert "BRIDGE_SELECTION_CLICK_SUPPRESS_MS" in button_up
    assert "atomic_exchange(&s_bridge_selection_confirming, true)" in confirm
    assert "queue_bridge_control(BRIDGE_CONTROL_CONFIRM)" in confirm
    assert "static void bridge_control_task" in source
    assert 'xTaskCreatePinnedToCore(bridge_control_task, "bridge_control"' in source
    assert "#define BRIDGE_SELECTION_CONFIRM_HOLD_MS 1500" in source
    assert ".press_time = BRIDGE_SELECTION_CONFIRM_HOLD_MS" in init_button
    assert "bridge_selection_confirm_long_cb" in init_button
    assert '"CONFIRMING"' in source
    assert '"CONFIRMED"' in source


def test_full_scan_uses_probe_lock_but_saved_bridge_switch_does_not() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    discovery = source.split("static size_t bridge_discover_subnet_profiles", 1)[1]
    discovery = discovery.split("static bool bridge_discovered_profile_equal", 1)[0]
    cycle = source.split("static void cycle_bridge_profile(void)\n{", 1)[1]
    cycle = cycle.split("static esp_err_t bridge_prepare_active_target", 1)[0]

    assert "static SemaphoreHandle_t s_bridge_probe_lock;" in source
    assert "s_bridge_probe_lock = xSemaphoreCreateMutex();" in source
    assert "static void bridge_probe_lock(void)" in source
    assert "static void bridge_probe_unlock(void)" in source
    assert "bridge_probe_lock();" in discovery
    assert "bridge_probe_unlock();" in discovery
    assert discovery.index("bridge_probe_lock();") < discovery.index(
        "bridge_wait_for_socket_connections("
    )
    assert discovery.index("bridge_wait_for_socket_connections(") < discovery.index(
        "bridge_probe_unlock();"
    )
    assert "bridge_probe_lock();" not in cycle
    assert "bridge_probe_unlock();" not in cycle
    assert "socket(" not in cycle


def test_bridge_profile_store_access_uses_lock_and_snapshots() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    snapshot_at = source.split("static bool bridge_profile_snapshot_at", 1)[1]
    snapshot_at = snapshot_at.split("static bool bridge_target_profile_snapshot", 1)[0]
    saved_snapshot = source.split("static bool bridge_saved_profile_snapshot_at", 1)[1]
    saved_snapshot = saved_snapshot.split("static bool bridge_profile_snapshot_at", 1)[0]
    merge = source.split("static bool bridge_profiles_merge_scan_results", 1)[1]
    merge = merge.split("static void bridge_discovery_task", 1)[0]
    cycle = source.split("static void cycle_bridge_profile(void)\n{", 1)[1]
    cycle = cycle.split("static esp_err_t bridge_prepare_active_target", 1)[0]

    assert "static SemaphoreHandle_t s_bridge_profiles_lock;" in source
    assert "s_bridge_profiles_lock = xSemaphoreCreateMutex();" in source
    assert "static void bridge_profiles_lock(void)" in source
    assert "static void bridge_profiles_unlock(void)" in source
    assert "bridge_profiles_lock();" in snapshot_at
    assert "bridge_profiles_unlock();" in snapshot_at
    assert "bridge_profile_snapshot_from_discovered" in snapshot_at
    assert "bridge_profiles_lock();" in merge
    assert "bridge_profile_views_rebuild();" in merge
    assert "bridge_profiles_unlock();" in merge
    assert merge.index("bridge_profiles_unlock();") < merge.index(
        "bridge_profiles_save_nvs(scan_ssid)"
    )
    assert "bridge_profiles_lock();" in saved_snapshot
    assert "bridge_profile_snapshot_from_discovered" in saved_snapshot
    assert "bridge_saved_profile_snapshot_at(index, &profile)" in cycle
    assert "bridge_saved_profile_snapshot_at(next_index, &next)" in cycle


def test_background_merge_keeps_active_target_and_wifi_identity_stable() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    target_lookup = source.split("static bool bridge_target_profile_snapshot", 1)[1]
    target_lookup = target_lookup.split("static void bridge_profile_views_rebuild", 1)[0]
    discovery = source.split("static size_t bridge_discover_subnet_profiles", 1)[1]
    discovery = discovery.split("static bool bridge_discovered_profile_equal", 1)[0]
    merge = source.split("static bool bridge_profiles_merge_scan_results", 1)[1]
    merge = merge.split("static void bridge_discovery_task", 1)[0]
    task = source.split("static void bridge_discovery_task", 1)[1]
    task = task.split("static bool start_bridge_discovery_task", 1)[0]

    assert "strcmp(profile->id, target->profile_id) == 0" in target_lookup
    assert "k_configured_bridge_profiles" in target_lookup
    assert "target->profile_index" not in target_lookup
    assert "current_wifi_ssid(scan_ssid" in discovery
    assert "bridge_profiles_merge_scan_results" not in discovery
    assert "bridge_profiles_merge_scan_results(scan_ssid)" in task
    assert "strcmp(current_ssid, scan_ssid) != 0" in merge
    assert "bridge_profiles_save_nvs(scan_ssid)" in merge


def test_rediscovered_bridge_address_refreshes_active_target() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    prepare = source.split("static esp_err_t bridge_prepare_active_target", 1)[1]
    prepare = prepare.split("static esp_err_t http_request_timeout", 1)[0]

    assert "bridge_profile_index_by_id(profile.id)" in prepare
    assert "strcmp(current.host, profile.host) != 0" in prepare
    assert "current.port != profile.port" in prepare
    assert '"rediscovered", true' in prepare
    assert "bridge_target_save_nvs()" in prepare
    assert "bridge target refreshed id=%s host=%s port=%d" in prepare


def test_concurrent_bridge_switch_ignores_stale_network_results() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    note_result = source.split("static void bridge_target_note_result", 1)[1]
    note_result = note_result.split("static bool bridge_target_needs_selection", 1)[0]
    request = source.split("static esp_err_t http_request_timeout", 1)[1]
    request = request.split("static esp_err_t http_request(", 1)[0]

    assert "expected_profile_id" in note_result
    assert "strcmp(s_bridge_target.profile_id, expected_profile_id) != 0" in note_result
    assert "bridge result ignored for stale profile id=%s" in note_result
    assert "bridge_target_note_result(target.profile_id, err);" in request


def test_background_scan_uses_atomic_recording_lifecycle_flags() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    busy = source.split("static bool recording_network_busy", 1)[1]
    busy = busy.split("static agent_state_t", 1)[0]

    assert "static atomic_bool s_recording_session_active;" in source
    assert "static atomic_bool s_recording_upload_active;" in source
    assert "atomic_load(&s_recording_session_active)" in busy
    assert "atomic_load(&s_recording_upload_active)" in busy
    assert "s_recording_session_id[0]" not in busy
    assert "s_recording_upload_task" not in busy
    assert "set_recording_session_active(true);" in source
    assert "set_recording_session_active(false);" in source
    assert "set_recording_upload_active(true);" in source
    assert "set_recording_upload_active(false);" in source
    assert "bridge_profile_at(" not in source
    assert "bridge_target_profile(&" not in source


def test_bridge_background_scan_pauses_for_recording_and_does_not_auto_switch_current_target() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    discovery = source.split("static size_t bridge_discover_subnet_profiles", 1)[1]
    discovery = discovery.split("static bool bridge_discovered_profile_equal", 1)[0]
    task = source.split("static void bridge_discovery_task", 1)[1]
    task = task.split("static bool start_bridge_discovery_task", 1)[0]

    assert "while (recording_network_busy())" in discovery
    assert "bridge discovery paused while recording network is busy" in discovery
    assert "bridge_target_set_profile" not in task
    assert '"manual-search"' not in task
    assert "bridge_profiles_merge_scan_results(scan_ssid)" in task
    assert "atomic_load(&s_bridge_selection_active)" in task


def test_discovery_supports_legacy_and_generic_bridge_identity() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    parser = source.split("static bool bridge_parse_discovered_health", 1)[1]
    parser = parser.split("static bool bridge_probe_discovered", 1)[0]
    discovery_probe = source.split("static bool bridge_probe_discovered", 1)[1]
    discovery_probe = discovery_probe.split("static bool bridge_probe_profile", 1)[0]
    profile_probe = source.split("static bool bridge_probe_profile", 1)[1]
    profile_probe = profile_probe.split("static bool bridge_scan_add", 1)[0]

    assert "#define BRIDGE_HEALTH_RESPONSE_BYTES 512" in source
    assert 'cJSON_GetObjectItemCaseSensitive(root, "bridge_name")' in parser
    assert 'cJSON_GetObjectItemCaseSensitive(root, "bridge_id")' in parser
    assert '"capswriter-m5-voice-bridge"' in parser
    assert "bridge_discovery_fallback_id(host" in parser
    assert "char response[BRIDGE_HEALTH_RESPONSE_BYTES] = {0};" in discovery_probe
    assert "char response[BRIDGE_HEALTH_RESPONSE_BYTES] = {0};" in profile_probe
    assert "char response[160]" not in profile_probe
    assert 'strncmp(profile->id, "lan-", 4) == 0' in profile_probe


def test_serial_debug_command_uses_the_side_button_event_path() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    serial_task = source.split("static void serial_debug_task", 1)[1]
    serial_task = serial_task.split("void app_main", 1)[0]

    assert "esp_rom_output_rx_one_char(&input)" in serial_task
    assert "usb_serial_jtag_driver_install(&usb_config)" in serial_task
    assert "usb_serial_jtag_read_bytes(&input, 1" in serial_task
    assert "serial debug command: side button full scan" in serial_task
    assert "side_button_up_cb(NULL, NULL)" in serial_task
    assert "serial debug command: clear runtime bridge profiles" in serial_task
    assert "bridge_profiles_clear()" in serial_task
    assert "serial debug command: front button short press" in serial_task
    assert "button_press_down_cb(NULL, NULL)" in serial_task
    assert "button_up_cb(NULL, NULL)" in serial_task
    assert "button_single_click_cb(NULL, NULL)" in serial_task
    assert "serial debug command: front button 1.5s hold" in serial_task
    assert "bridge_selection_confirm_long_cb(NULL, NULL)" in serial_task
    assert "bridge selection confirmation visible" in source
    assert "bridge selection mode exited" in source
    assert 'xTaskCreate(serial_debug_task, "serial_debug", 6144' in source


def test_s3_uses_a_softer_recording_tone_profile_while_plus_keeps_its_buzzer_profile() -> None:
    source = AUDIO_C.read_text(encoding="utf-8")
    recording_start = source.split("static const sound_segment_t recording_start[] = {", 1)[1]
    recording_start = recording_start.split("};", 1)[0]
    recording_stop = source.split("static const sound_segment_t recording_stop[] = {", 1)[1]
    recording_stop = recording_stop.split("};", 1)[0]

    profile_block = source.split("#define VIBE_STICK_AUDIO_CORE 1\n\n", 1)[1]
    profile_block = profile_block.split("\ntypedef struct", 1)[0]
    s3_profile = profile_block.split("#if VIBE_BOARD_HAS_ES8311", 1)[1].split("#else", 1)[0]
    plus_profile = profile_block.split("#else", 1)[1].split("#endif", 1)[0]

    assert "#define VIBE_STICK_SOUND_VOLUME 0.28f" in s3_profile
    assert "#define VIBE_STICK_SOUND_OUTPUT_VOLUME 70" in s3_profile
    assert "#define VIBE_STICK_RECORDING_START_HIGH_HZ 1800" in s3_profile
    assert "#define VIBE_STICK_RECORDING_STOP_HZ 1500" in s3_profile
    assert "#define VIBE_STICK_RECORDING_START_HIGH_HZ 3600" in plus_profile
    assert "#define VIBE_STICK_RECORDING_STOP_HZ 4000" in plus_profile
    assert "{.freq_hz = VIBE_STICK_RECORDING_START_HIGH_HZ" in recording_start
    assert "{.freq_hz = VIBE_STICK_RECORDING_START_LOW_HZ" in recording_start
    assert "{.freq_hz = VIBE_STICK_RECORDING_STOP_HZ" in recording_stop
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
    assert "VIBE_STICK_ESCAPE_GLITCH_SHORT_MS" in source
    assert "VIBE_STICK_ESCAPE_GLITCH_LONG_MS" in source
    assert "VIBE_STICK_ESCAPE_GLITCH_GAP_MS" in source
    assert "{.freq_hz = VIBE_STICK_FOLLOWUP_ENTER_LOW_HZ" in followup_enter
    assert "{.freq_hz = VIBE_STICK_FOLLOWUP_ENTER_HIGH_HZ" in followup_enter
    assert "{.freq_hz = VIBE_STICK_FOLLOWUP_ESCAPE_HIGH_HZ" in followup_escape
    assert "{.freq_hz = 760, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_LONG_MS}" in followup_escape
    assert "{.freq_hz = VIBE_STICK_FOLLOWUP_ESCAPE_MID_HZ" in followup_escape
    assert "{.freq_hz = 520, .duration_ms = VIBE_STICK_ESCAPE_GLITCH_LONG_MS}" in followup_escape
    assert followup_enter != followup_escape
    assert 'start_ptt_followup_key_dispatch("button_followup_enter"' in main_source
    assert 'start_ptt_followup_key_dispatch("button_followup_escape"' in main_source


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


def test_battery_level_uses_voltage_curve_and_voltage_api() -> None:
    board_source = BOARD_C.read_text(encoding="utf-8")
    board_header = BOARD_H.read_text(encoding="utf-8")

    assert "esp_err_t vibe_board_battery_voltage_mv(int *voltage_mv);" in board_header
    assert "esp_err_t vibe_board_battery_voltage_mv(int *voltage_mv)" in board_source
    assert "{3082, 0}" in board_source
    assert "{4042, 100}" in board_source
    assert "int level = (voltage_mv - 3300) * 100 / (4150 - 3350);" not in board_source
    assert "vibe_board_battery_voltage_mv(&voltage_mv)" in board_source


def test_battery_display_filters_raw_voltage_status() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "VIBE_STICK_BATTERY_SAMPLE_COUNT 5" in source
    assert "VIBE_STICK_BATTERY_USB_UNPLUG_HOLD_MS 30000" in source
    assert "VIBE_STICK_BATTERY_WAKE_STABILIZE_MS 5000" in source
    assert "median_battery_sample()" in source
    assert "battery_drop_hold_active(now_ms)" in source
    assert "RTC_DATA_ATTR static int s_retained_battery_display_level" in source
    assert "s_retained_battery_magic == VIBE_STICK_BATTERY_RTC_MAGIC" in source
    assert "s_deep_sleep_wake_ms = esp_timer_get_time() / 1000;" in source
    assert "vibe_board_battery_voltage_mv(&battery_voltage_mv)" in source
    assert "power status battery_raw=%d battery_display=%d battery_mv=%d charging=%d usb=%d" in source


def test_battery_ui_uses_color_bands_without_a_percentage_label() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    battery_ui = source.split("static void set_battery_ui", 1)[1]
    battery_ui = battery_ui.split("typedef struct {", 1)[0]

    assert "BATTERY_LOW_THRESHOLD_PERCENT 20" in source
    assert "BATTERY_HIGH_THRESHOLD_PERCENT 50" in source
    assert "s_battery_label" not in source
    assert 'snprintf(battery' not in battery_ui
    assert "battery_value < BATTERY_LOW_THRESHOLD_PERCENT" in battery_ui
    assert "battery_value < BATTERY_HIGH_THRESHOLD_PERCENT" in battery_ui
    assert "0xef4444" in battery_ui
    assert "0xfacc15" in battery_ui
    assert "0x32d583" in battery_ui
    assert "lv_obj_set_style_border_color(s_battery_icon, battery_color, 0);" in battery_ui
    assert "lv_obj_set_style_bg_color(s_battery_fill, battery_color, 0);" in battery_ui


def test_state_polling_stops_while_screen_is_off() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_display_power_state == DISPLAY_POWER_ACTIVE" in source
    assert "now_ms - last_poll >= state_poll_interval_ms(now_ms)" in source
    assert "VIBE_STICK_STATE_POLL_IDLE_MS 10000" in source
    assert "VIBE_STICK_IDLE_STATE_POLL_MS" not in source


def test_ota_check_blocks_sleep_without_waking_display() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    display_guard = source.split("static bool display_should_stay_active(void)", 1)[1]
    display_guard = display_guard.split("static bool deep_sleep_should_stay_awake(void)", 1)[0]

    assert "ota_in_progress()" not in display_guard
    assert "static bool deep_sleep_should_stay_awake(void)" in source
    assert "return display_should_stay_active() || ota_in_progress();" in source
    assert "deep_sleep_should_stay_awake() ||" in source


def test_ota_check_runs_on_network_wake_and_periodically() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    config = (ROOT / "firmware" / "sticks3" / "include" / "vibe_stick_config.h").read_text(
        encoding="utf-8"
    )

    assert "queue_event(VIBE_STICK_EVENT_OTA_CHECK);" in source
    assert "case VIBE_STICK_EVENT_OTA_CHECK:" in source
    assert "start_ota_check_task();" in source
    assert "#define OTA_PERIODIC_CHECK_MS 300000" in source
    assert "#define OTA_BATTERY_CHECK_MS 1800000" in source
    assert "s_last_ota_check_ms" in source
    assert "now_ms - s_last_ota_check_ms >= ota_interval_ms" in source
    assert "ota_power_policy_allows" in source
    assert "VIBE_STICK_OTA_CHECK_MS" not in config


def test_ota_rejects_lower_semantic_versions_before_hash_comparison() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    ota_check = source.split("static bool ota_manifest_is_new", 1)[1]
    ota_check = ota_check.split("static esp_err_t perform_ota_update", 1)[0]

    assert "static bool ota_parse_semantic_version" in source
    assert "static bool ota_compare_semantic_versions" in source
    assert "ota_compare_semantic_versions(manifest->version, FIRMWARE_VERSION" in ota_check
    assert "version_comparison < 0" in ota_check
    assert "OTA manifest version is older" in ota_check
    assert "version_comparison == 0" in ota_check
    assert "OTA manifest version is not newer" in ota_check
    assert ota_check.index("version_comparison < 0") < ota_check.index(
        "if (manifest->sha256[0] != '\\0')"
    )
    assert ota_check.index("version_comparison == 0") < ota_check.index(
        "if (manifest->sha256[0] != '\\0')"
    )


def test_lift_motion_start_is_deferred_instead_of_dropped() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_motion_start_pending" in source
    assert "request_motion_recording_start()" in source
    assert "motion lift start deferred while recording network is busy" in source
    assert "motion lift start deferred request cancelled by flat posture" in source
    assert "queue_event(VIBE_STICK_EVENT_MOTION_START)" in source


def test_deep_sleep_keeps_button_wake_and_guards_lift_mode() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    motion_source = (ROOT / "firmware/sticks3/src/vibe_motion.c").read_text(encoding="utf-8")
    motion_header = (ROOT / "firmware/sticks3/include/vibe_motion.h").read_text(encoding="utf-8")
    board_profile = BOARD_PROFILE_H.read_text(encoding="utf-8")
    plus_profile = board_profile.split("#else", 1)[0]

    assert "VIBE_STICK_DEEP_SLEEP_MS 300000" in source
    assert "maybe_enter_deep_sleep(now_ms)" in source
    assert "esp_deep_sleep_start()" in source
    assert "esp_sleep_enable_ext0_wakeup(ext0_gpio, 0)" in source
    assert "gpio_num_t ext0_gpio = VIBE_BOARD_PIN_BUTTON_FRONT;" in source
    assert "ext0_gpio = VIBE_BOARD_PIN_IMU_INT;" not in source
    assert "esp_sleep_enable_ext1_wakeup_io(wake_mask" in source
    assert "static bool sleep_wake_gpio_is_active(gpio_num_t gpio)" in source
    assert "static bool sleep_wake_mask_contains(uint64_t wake_mask, gpio_num_t gpio)" in source
    assert "deep sleep skipped: ext0 wake gpio=%d is already active" in source
    assert "sleep_wake_gpio_is_active(ext0_gpio)" in source
    assert "vibe_motion_prepare_deep_sleep()" in source
    assert "vibe_motion_prepare_deep_sleep_wake()" in source
    assert "esp_err_t vibe_motion_prepare_deep_sleep(void);" in motion_header
    assert "write_reg(MPU6886_INT_ENABLE, 0)" in motion_source
    assert "write_reg(MPU6886_PWR_MGMT_2, MPU6886_AXIS_STANDBY_MASK)" in motion_source
    assert "write_reg(MPU6886_PWR_MGMT_1, MPU6886_SLEEP_MODE)" in motion_source
    assert "read_regs(MPU6886_INT_STATUS, &reg, 1)" in motion_source
    assert "#define VIBE_BOARD_PIN_IMU_INT GPIO_NUM_35" in board_profile
    assert "#define VIBE_BOARD_HAS_IMU_DEEP_SLEEP_WAKE 0" in plus_profile
    assert "#define VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL 1" in board_profile
    assert ".disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL" in source


def test_recording_mode_preference_survives_deep_sleep_restart() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert 'DEVICE_PREF_NAMESPACE "vibe_prefs"' in source
    assert 'DEVICE_PREF_RECORDING_MODE_KEY "rec_mode"' in source
    assert 'DEVICE_PREF_RECORDING_TRIGGER_KEY "rec_trig"' in source
    assert 'DEVICE_PREF_RECORDING_INTENT_KEY "rec_intent"' in source
    assert "save_recording_mode_preference()" in source
    assert "restore_recording_mode_preference()" in source
    assert "nvs_get_u8(handle, DEVICE_PREF_RECORDING_TRIGGER_KEY" in source
    assert "nvs_get_u8(handle, DEVICE_PREF_RECORDING_INTENT_KEY" in source
    assert "nvs_get_u8(handle, DEVICE_PREF_RECORDING_MODE_KEY" in source
    assert "vibe_motion_recalibrate()" in source


def test_recording_trigger_is_independent_from_disabled_cyber_intents() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    board_profile = BOARD_PROFILE_H.read_text(encoding="utf-8")
    toggle = source.split("static void toggle_recording_mode(void)", 1)[1]
    toggle = toggle.split("static void toggle_recording_intent(void)", 1)[0]
    intent_toggle = source.split("static void toggle_recording_intent(void)", 1)[1]
    intent_toggle = intent_toggle.split("static void lvgl_lock", 1)[0]
    migration = source.split("static void migrate_legacy_recording_mode", 1)[1]
    migration = migration.split("static esp_err_t restore_recording_mode_preference", 1)[0]
    init_button = source.split("static esp_err_t init_button", 1)[1]
    init_button = init_button.split("static void restore_wake_button_intent", 1)[0]
    side_button_setup = init_button.split(
        "iot_button_new_gpio_device(&button_config, &side_gpio_config", 1
    )[1]

    assert "VIBE_STICK_ANIM_PREVIEW 0" in source
    assert board_profile.count("#define VIBE_BOARD_HAS_CYBER_INTENTS 0") == 2
    assert "recording_intent_supported" in source
    assert "sanitize_recording_intent();" in source
    assert "cyber intents unavailable" in intent_toggle
    assert "s_recording_trigger_mode = RECORDING_TRIGGER_LIFT_TO_TALK;" in source
    assert "s_recording_trigger_mode = RECORDING_TRIGGER_PUSH_TO_TALK;" in source
    assert "s_recording_intent = RECORDING_INTENT_DICTATION;" in intent_toggle
    assert "RECORDING_INTENT_CYBER_FORTUNE" not in toggle
    assert "case RECORDING_MODE_CYBER_FORTUNE:" in migration
    assert "case RECORDING_MODE_CYBER_ALMANAC:" in migration
    assert "VIBE_STICK_EVENT_RECORDING_INTENT_TOGGLE" in source
    assert "side_button_double_click_cb" not in source
    assert "BUTTON_DOUBLE_CLICK, NULL" not in side_button_setup


def test_side_mode_switches_show_large_main_screen_visual_feedback() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    toggle = source.split("static void toggle_recording_mode(void)", 1)[1]
    toggle = toggle.split("static void toggle_recording_intent(void)", 1)[0]
    intent_toggle = source.split("static void toggle_recording_intent(void)", 1)[1]
    intent_toggle = intent_toggle.split("static void lvgl_lock", 1)[0]
    update_pet = source.split("static void update_pet_visual(void)", 1)[1]
    update_pet = update_pet.split("#if VIBE_STICK_ANIM_PREVIEW", 1)[0]

    assert "VIBE_STICK_MODE_SWITCH_VISUAL_MS 1800" in source
    assert "s_mode_switch_layer = lv_obj_create(screen);" in source
    assert 'make_label(s_mode_switch_layer, "DICTATION",' in source
    assert '"PUSH TO TALK"' in source
    assert '"LIFT TO TALK"' in source
    assert '"FORTUNE"' in source
    assert '"ALMANAC"' in source
    assert "show_trigger_mode_switch_visual();" in toggle
    assert "show_recording_intent_switch_visual();" in intent_toggle
    assert "mode_switch_visual_active(now_ms)" in update_pet
    assert "set_pet_frame(s_mode_switch_frames[s_mode_switch_frame_index])" in update_pet
    assert "finish_mode_switch_visual();" in update_pet


def test_deep_sleep_button_wake_restores_ptt_hold() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "s_woke_from_deep_sleep = wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED;" in source
    assert "capture_deep_sleep_front_button_intent()" in source
    assert "handle_deep_sleep_front_button_intent()" in source
    assert "front button held during deep sleep wake; pending PTT restore" in source
    assert "restoring front long press after deep sleep wake" in source
    assert "s_wake_front_button_pending = false;" in source
    assert 'handle_recording_start("button_long_start", "RELEASE TO SEND")' in source


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


def test_deep_sleep_validates_wake_gpio_before_wifi_shutdown() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    sleep = source.split("static bool enter_deep_sleep(void)", 1)[1]
    sleep = sleep.split("static void maybe_enter_deep_sleep", 1)[0]

    assert sleep.index("sleep_wake_gpio_is_active(ext0_gpio)") < sleep.index("esp_wifi_stop()")
    assert sleep.index("sleep_wake_gpio_is_active(VIBE_BOARD_PIN_BUTTON_FRONT)") < sleep.index(
        "esp_wifi_stop()"
    )
    assert "ext0_gpio = VIBE_BOARD_PIN_IMU_INT;" not in sleep


def test_deep_sleep_retry_uses_delayed_backoff_after_active_wake_gpio() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    maybe_sleep = source.split("static void maybe_enter_deep_sleep", 1)[1]
    maybe_sleep = maybe_sleep.split("static esp_err_t init_display", 1)[0]

    assert "VIBE_STICK_DEEP_SLEEP_RETRY_MS" in source
    assert "s_next_deep_sleep_attempt_ms" in source
    assert "now_ms + VIBE_STICK_DEEP_SLEEP_RETRY_MS" in maybe_sleep
    assert "now_ms < s_next_deep_sleep_attempt_ms" in maybe_sleep


def test_s3_deep_sleep_wake_gpio_preserves_internal_button_pullups() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    board_profile = BOARD_PROFILE_H.read_text(encoding="utf-8")
    sleep = source.split("static bool enter_deep_sleep(void)", 1)[1]
    sleep = sleep.split("static void maybe_enter_deep_sleep", 1)[0]
    init_button = source.split("static esp_err_t init_button", 1)[1]
    init_button = init_button.split("static void capture_deep_sleep_front_button_intent", 1)[0]

    assert "#define VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL 0" in board_profile
    assert ".disable_pull = VIBE_BOARD_BUTTONS_DISABLE_INTERNAL_PULL" in init_button
    assert "configure_sleep_wake_gpio(VIBE_BOARD_PIN_BUTTON_FRONT" not in sleep
    assert "configure_sleep_wake_gpio(VIBE_BOARD_PIN_BUTTON_SIDE" not in sleep


def test_side_button_gpio_keeps_power_save_enabled() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    init_button = source.split("static esp_err_t init_button", 1)[1]
    init_button = init_button.split("static void capture_deep_sleep_front_button_intent", 1)[0]
    side_gpio = init_button.split("const button_gpio_config_t side_gpio_config", 1)[1]
    side_gpio = side_gpio.split("};", 1)[0]

    assert ".enable_power_save = true" in side_gpio


def test_ptt_recording_suspends_motion_and_lift_mode_resumes_it() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    motion_header = (ROOT / "firmware/sticks3/include/vibe_motion.h").read_text(encoding="utf-8")
    ptt_mode = source.split("static void set_push_to_talk_trigger_mode", 1)[1]
    ptt_mode = ptt_mode.split("static esp_err_t set_lift_to_talk_trigger_mode", 1)[0]
    lift_mode = source.split("static esp_err_t set_lift_to_talk_trigger_mode", 1)[1]
    lift_mode = lift_mode.split("static esp_err_t save_recording_mode_preference", 1)[0]

    assert "esp_err_t vibe_motion_suspend(void);" in motion_header
    assert "esp_err_t vibe_motion_resume(void);" in motion_header
    assert "vibe_motion_suspend()" in ptt_mode
    assert "vibe_motion_resume()" in lift_mode


def test_motion_calibration_has_finite_timeout_and_fallback_baseline() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    lift_mode = source.split("static esp_err_t set_lift_to_talk_trigger_mode", 1)[1]
    lift_mode = lift_mode.split("static esp_err_t save_recording_mode_preference", 1)[0]
    timeout = source.split("static void maybe_timeout_motion_calibration", 1)[1]
    timeout = timeout.split("static uint32_t state_poll_interval_ms", 1)[0]

    assert "VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS 15000" in source
    assert "s_motion_calibration_deadline_ms" in source
    assert "VIBE_STICK_MOTION_CALIBRATION_TIMEOUT_MS" in lift_mode
    assert "lift calibration timed out; falling back to PTT" in timeout
    assert "set_push_to_talk_trigger_mode();" in timeout
    assert "save_recording_mode_preference()" in timeout


def test_audio_stop_unblocks_bounded_recording_reads_before_waiting_for_task_exit() -> None:
    source = AUDIO_C.read_text(encoding="utf-8")
    read_chunk = source.split("static esp_err_t read_audio_chunk", 1)[1]
    read_chunk = read_chunk.split("static void audio_task", 1)[0]
    stop = source.split("esp_err_t vibe_audio_stop(void)", 1)[1]
    stop = stop.split("bool vibe_audio_is_recording", 1)[0]

    assert "AUDIO_READ_WAIT_MS" in source
    assert "portMAX_DELAY" not in read_chunk
    assert "AUDIO_READ_WAIT_MS" in read_chunk
    assert "esp_codec_dev_read(s_codec" in read_chunk
    assert stop.index("atomic_store(&s_running, false)") < stop.index("while (s_audio_task != NULL)")
    assert "signal_capture_stop_locked()" in stop
    assert "forcing bounded cleanup" in stop
    assert "vTaskDelete(s_audio_task)" in stop


def test_release_firmware_can_disable_serial_debug_input_task() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    serial_task = source.split("#if VIBE_STICK_SERIAL_DEBUG_ENABLED", 1)[1]
    serial_task = serial_task.split("void app_main", 1)[0]
    app_main = source.split("void app_main(void)", 1)[1]

    assert "VIBE_STICK_SERIAL_DEBUG_ENABLED" in source
    assert "static void serial_debug_task" in serial_task
    assert "#if VIBE_STICK_SERIAL_DEBUG_ENABLED" in app_main
    assert 'xTaskCreate(serial_debug_task, "serial_debug", 6144' in app_main


def test_display_off_suspends_panel_output_and_lvgl_timer_work() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    power = source.split("static void update_power_saving", 1)[1]
    power = power.split("static void request_motion_recording_start", 1)[0]
    register_activity = source.split("static void register_activity(void)\n{", 1)[1]
    register_activity = register_activity.split("static void update_power_saving", 1)[0]
    display_suspend = source.split("static void set_display_rendering_suspended", 1)[1]
    display_suspend = display_suspend.split("static void fade_backlight_toward", 1)[0]

    assert "set_display_rendering_suspended(true)" in power
    assert "set_display_rendering_suspended(false)" in register_activity
    assert "esp_lcd_panel_disp_on_off(s_panel, false)" in display_suspend
    assert "lv_timer_pause(s_pet_timer)" in display_suspend
    assert "lv_timer_resume(s_pet_timer)" in display_suspend
    assert "esp_lcd_panel_disp_on_off(s_panel, true)" in display_suspend
    panel_off_index = display_suspend.index(
        "esp_lcd_panel_disp_on_off(s_panel, false)"
    )
    assert "#if VIBE_BOARD_HAS_GPIO_BACKLIGHT" not in display_suspend[
        max(0, panel_off_index - 80) : panel_off_index
    ]


def test_wifi_reconnect_uses_delayed_backoff_instead_of_immediate_retry() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    handler = source.split("static void wifi_event_handler", 1)[1]
    handler = handler.split("static esp_err_t init_wifi", 1)[0]
    disconnect = handler.split("WIFI_EVENT_STA_DISCONNECTED", 1)[1]
    disconnect = disconnect.split("} else if (event_base == IP_EVENT", 1)[0]

    assert "wifi_reconnect_delay_ms" in source
    assert "s_wifi_reconnect_timer" in source
    assert "schedule_wifi_reconnect();" in disconnect
    assert "ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect())" not in disconnect


def test_power_management_and_tickless_idle_are_enabled_by_default() -> None:
    defaults = (ROOT / "firmware/sticks3/sdkconfig.defaults").read_text(encoding="utf-8")
    source = MAIN_C.read_text(encoding="utf-8")

    assert "CONFIG_PM_ENABLE=y" in defaults
    assert "CONFIG_PM_DFS_INIT_AUTO=n" in defaults
    assert "CONFIG_FREERTOS_USE_TICKLESS_IDLE=y" in defaults
    assert "esp_pm_configure(&config)" in source
    assert ".light_sleep_enable = true" in source


def test_tts_playback_probe_reports_device_result() -> None:
    source = MAIN_C.read_text(encoding="utf-8")

    assert "tts_playback_request_id" in source
    assert "VIBE_STICK_EVENT_TTS_PROBE" in source
    assert "tts_probe_played" in source
    assert "tts_probe_failed" in source
    assert "post_recording_playback_event" in source


def test_cyber_processing_keeps_overlay_until_tts_probe() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    finish_stop = source.split("static void finish_recording_stop", 1)[1]
    finish_stop = finish_stop.split("static void recording_finalize_task", 1)[0]
    tts_probe = source.split("case VIBE_STICK_EVENT_TTS_PROBE", 1)[1]
    tts_probe = tts_probe.split("case VIBE_STICK_EVENT_OTA_CHECK", 1)[0]

    assert "VIBE_STICK_CYBER_TTS_WAIT_TIMEOUT_MS" in source
    assert "s_cyber_tts_waiting" in source
    assert 'strcmp(recording_status, "cyber_processing") == 0' in finish_stop
    assert "start_cyber_tts_wait();" in finish_stop
    assert "if (!s_cyber_tts_waiting)" in finish_stop
    assert 'show_recording_overlay("SENDING", "", true);' in source
    assert "maybe_timeout_cyber_tts_wait(now_ms);" in source
    assert "clear_cyber_tts_wait();" in tts_probe


def test_firmware_runtime_ui_uses_ascii_only() -> None:
    source = MAIN_C.read_text(encoding="utf-8")
    cmake = (ROOT / "firmware" / "sticks3" / "src" / "CMakeLists.txt").read_text(encoding="utf-8")

    assert re.search(r"[\u4e00-\u9fff]", source) is None
    assert "FONT_CN" not in source
    assert "vibe_stick_cn_16" not in cmake
