from pathlib import Path


SOURCE = (
    Path(__file__).resolve().parents[1]
    / "firmware/sticks3/src/vibe_cardputer_messages.c"
)


def test_cardputer_message_http_runs_only_in_message_mode() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    sync_once = source.split("static void sync_once(void)", 1)[1]
    sync_once = sync_once.split("static void sync_task", 1)[0]
    sync_task = source.split("static void sync_task", 1)[1]
    sync_task = sync_task.split("static const lv_font_t", 1)[0]

    assert "!atomic_load(&s_storage_ready) || !atomic_load(&s_active)" in sync_once
    assert "if (!atomic_load(&s_active)) return;" in sync_once
    assert "if (action == MESSAGE_ACTION_OPEN)" in sync_task
    assert "next_sync_ms = 0;" in sync_task
    assert "if (atomic_load(&s_active) && now_ms >= next_sync_ms)" in sync_task
    assert "if (!atomic_load(&s_active)) sync_once();" not in sync_task
