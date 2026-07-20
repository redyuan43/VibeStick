from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_python_package_exposes_only_telemetry_commands() -> None:
    pyproject = (ROOT / "bridge" / "pyproject.toml").read_text(encoding="utf-8")
    main = (
        ROOT / "bridge" / "src" / "vibe_stick" / "__main__.py"
    ).read_text(encoding="utf-8")

    assert "vibestick-bridge" not in pyproject
    assert "zeroconf" not in pyproject
    assert 'vibestick-battery = "vibe_stick.telemetry.cli:main"' in pyproject
    assert 'vibestick-telemetry = "vibe_stick.telemetry.server:main"' in pyproject
    assert "from vibe_stick.telemetry.server import main" in main
    assert "vibe_stick.server" not in main


def test_installer_launches_only_telemetry_on_8878() -> None:
    install = (ROOT / "scripts" / "install.sh").read_text(encoding="utf-8")
    uninstall = (ROOT / "scripts" / "uninstall.sh").read_text(encoding="utf-8")
    dev = (ROOT / "scripts" / "dev.sh").read_text(encoding="utf-8")
    doctor = (ROOT / "scripts" / "doctor.sh").read_text(encoding="utf-8")

    assert "com.vibestick.telemetry.plist" in install
    assert "vibe_stick.telemetry.server" in install
    assert "8878" in install
    assert "VibeStickHUD" not in install
    assert "com.vibestick.hud" not in install
    assert "com.vibestick.bridge" not in install
    assert "com.vibestick.telemetry.plist" in uninstall
    assert "com.vibestick.hud" not in uninstall
    assert "vibe_stick.telemetry.server" in dev
    assert "8878" in dev
    assert "127.0.0.1:8878/health" in doctor
    assert "vibe_stick.claude" not in doctor
    assert "check_asr" not in doctor
