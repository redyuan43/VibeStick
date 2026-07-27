from pathlib import Path


SOURCE = Path("firmware/sticks3/src/main_minijoy_bt.c").read_text()


def _function(name: str, next_name: str) -> str:
    return SOURCE.split(f"static void {name}", 1)[1].split(
        f"static void {next_name}", 1
    )[0]


def test_plus_board_status_led_is_active_low_gpio10() -> None:
    assert "#define BOARD_STATUS_LED_GPIO GPIO_NUM_10" in SOURCE
    assert "#define BOARD_STATUS_LED_ON_LEVEL 0" in SOURCE
    app_main = SOURCE.split("void app_main(void)", 1)[1]
    assert app_main.index("init_board_status_led()") < app_main.index(
        "vibe_bt_status_ui_init()"
    )


def test_pairing_blinks_board_and_minijoy_leds_from_one_timer() -> None:
    update = _function("update_status_leds", "open_minijoy")
    pairing = update.split("if (state.pairing)", 1)[1].split("return;", 1)[0]
    assert "set_minijoy_led(s_pairing_led_on ? MINIJOY_LED_PAIRING" in pairing
    assert "set_board_status_led(s_pairing_led_on);" in pairing
    assert pairing.count("s_pairing_led_toggle_ms") >= 3


def test_board_led_still_updates_when_minijoy_is_absent() -> None:
    update = _function("update_status_leds", "open_minijoy")
    assert "if (!s_minijoy_ready)" not in update
    assert "set_board_status_led(atomic_load(&s_capture_active)" in update
