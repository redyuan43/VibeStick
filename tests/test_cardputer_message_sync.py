from pathlib import Path


SOURCE = (
    Path(__file__).resolve().parents[1]
    / "firmware/sticks3/src/vibe_cardputer_messages.c"
)


def test_cardputer_message_http_runs_only_in_message_mode() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    sync_once = source.split("static void sync_once_inner(void)", 1)[1]
    sync_once = sync_once.split("static void sync_once(void)", 1)[0]
    sync_wrapper = source.split("static void sync_once(void)", 1)[1]
    sync_wrapper = sync_wrapper.split("static void sync_task", 1)[0]
    sync_task = source.split("static void sync_task", 1)[1]
    sync_task = sync_task.split("static const lv_font_t", 1)[0]

    assert "!atomic_load(&s_storage_ready) || !atomic_load(&s_active)" in sync_once
    assert "if (!atomic_load(&s_active)) return;" in sync_once
    assert "if (!atomic_load(&s_active)) {" in sync_once
    assert "atomic_compare_exchange_strong(&s_sync_in_progress" in sync_wrapper
    assert "sync_once_inner();" in sync_wrapper
    assert "atomic_store(&s_sync_in_progress, false);" in sync_wrapper
    assert "if (action == MESSAGE_ACTION_OPEN)" in sync_task
    assert "next_sync_ms = 0;" in sync_task
    assert "if (atomic_load(&s_active) && now_ms >= next_sync_ms)" in sync_task
    assert "if (!atomic_load(&s_active)) sync_once();" not in sync_task


def test_cardputer_message_mode_tracks_transitions_and_blocks_conflicting_input() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    handle_key = source.split(
        "bool vibe_cardputer_messages_handle_key", 1
    )[1].split("bool vibe_cardputer_messages_active", 1)[0]
    busy = source.split("bool vibe_cardputer_messages_busy(void)", 1)[1]
    busy = busy.split("bool vibe_cardputer_messages_storage_ready", 1)[0]
    actions = source.split("static void handle_action", 1)[1]
    actions = actions.split("esp_err_t vibe_cardputer_messages_init", 1)[0]

    assert "s_config.audio_busy && s_config.audio_busy()" in handle_key
    assert '"Fn+N ignored while Opt or recording is busy"' in handle_key
    assert "atomic_load(&s_ui_transition)" in handle_key
    assert "atomic_load(&s_sync_in_progress)" in handle_key
    assert "atomic_store(&s_ui_transition, true);" in handle_key
    assert "atomic_store(&s_ui_transition, false);" in handle_key
    assert "atomic_load(&s_active)" in busy
    assert "atomic_load(&s_ui_transition)" in busy
    assert "atomic_load(&s_sync_in_progress)" in busy
    assert actions.count("atomic_store(&s_ui_transition, false);") >= 2
