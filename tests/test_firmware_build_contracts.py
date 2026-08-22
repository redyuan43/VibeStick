import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_production_flash_script_enforces_115200_baud() -> None:
    script = (ROOT / "scripts" / "firmware.sh").read_text(encoding="utf-8")

    assert 'set -- -b 115200 "$@"' in script
    assert 'FLASH_BAUD" != "115200"' in script
    assert "fixed at 115200 baud for all boards" in script
    assert "*flash*)" in script
    assert 'FLASH_BAUD="${arg#-b}"' in script


def test_battery_firmware_flash_defaults_to_115200_baud() -> None:
    script = (ROOT / "scripts" / "battery-firmware.sh").read_text(
        encoding="utf-8"
    )

    assert script.count('flash_baud="${VIBE_STICK_FLASH_BAUD:-115200}"') == 2
    assert "Battery firmware flashing is fixed at 115200 baud." in script
    assert "460800" not in script


def test_four_board_size_baselines_match_ota_partition_layouts() -> None:
    baselines = json.loads(
        (
            ROOT / "firmware" / "sticks3" / "size-baseline.json"
        ).read_text(encoding="utf-8")
    )

    assert set(baselines) == {
        "sticks3",
        "stickc_plus",
        "stickc_plus_se",
        "cardputer_adv",
    }
    assert baselines["sticks3"]["ota_partition_size"] == 1800 * 1024
    assert baselines["cardputer_adv"]["ota_partition_size"] == 1800 * 1024
    assert baselines["stickc_plus"]["ota_partition_size"] == 1700 * 1024
    assert baselines["stickc_plus_se"]["ota_partition_size"] == 1700 * 1024
    assert all(
        item["baseline_size"] < item["ota_partition_size"]
        for item in baselines.values()
    )


def test_firmware_ci_builds_all_supported_boards() -> None:
    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(
        encoding="utf-8"
    )

    assert (
        "board: [sticks3, stickc_plus, stickc_plus_se, cardputer_adv]"
        in workflow
    )


def test_firmware_entrypoint_is_only_a_composition_handoff() -> None:
    entrypoint = (
        ROOT / "firmware" / "sticks3" / "src" / "main.c"
    ).read_text(encoding="utf-8")

    assert len(entrypoint.splitlines()) <= 1500
    assert "vibe_app_runtime_start();" in entrypoint
    for forbidden in ("cJSON", "nvs_", "lv_", "esp_http_client", "xTask"):
        assert forbidden not in entrypoint


def test_firmware_build_id_does_not_depend_on_compiler_clock() -> None:
    runtime = (
        ROOT / "firmware" / "sticks3" / "src" / "vibe_app_runtime.c"
    ).read_text(encoding="utf-8")
    publisher = (ROOT / "scripts" / "ota_publish.py").read_text(
        encoding="utf-8"
    )

    assert (
        '#define FIRMWARE_BUILD_ID VIBE_BOARD_NAME "-v" FIRMWARE_VERSION'
        in runtime
    )
    assert "__DATE__" not in runtime
    assert "__TIME__" not in runtime
    assert 'return f"{board}-v{version}" if version else board' in publisher


def test_release_script_requires_explicit_board_targets() -> None:
    script = (ROOT / "scripts" / "release-firmware.sh").read_text(
        encoding="utf-8"
    )

    assert "<board...|all>" in script
    assert "if [[ ${#BOARDS[@]} -eq 0 ]]" in script
    assert '"$ROOT_DIR/scripts/ota_publish.sh" "$board"' in script
    assert '"${VERIFY_ARGS[@]}" "${BOARDS[@]}"' in script
