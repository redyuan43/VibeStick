# Third-Party Audit

This audit documents the v0.1.1 repository state after cleanup.

| Project / file / dependency | Source | Current use | License status | Risk | Recommendation |
| --- | --- | --- | --- | --- | --- |
| `bridge/src/vibe_stick/` | Project-authored Python | Local Mac bridge, state API, quota observation, recording flow, ASR adapter, paste injection | MIT under this repository | Low | Keep. |
| `app/macos/VibeStickHUD/main.swift` | Project-authored Swift | Minimal recording status HUD | MIT under this repository | Low | Keep. |
| `firmware/sticks3/src/` and `firmware/sticks3/include/` | Project-authored C using ESP-IDF APIs | StickS3 UI, HTTP, buttons, audio, battery, speaker alerts | MIT under this repository | Low | Keep. |
| `assets/brand/vibestick-icon.svg` | Project-generated simple geometry | Temporary VibeStick brand icon | MIT under this repository | Low | Keep until polished branding exists. |
| `assets/providers/**` and `firmware/sticks3/assets/providers/**` | Project-generated simple geometry | Temporary provider/status icons | MIT under this repository | Low | Keep. Avoid replacing with third-party brand marks unless license/brand usage is reviewed. |
| `firmware/sticks3/generated/vibe_stick_ui_assets.c/.h` | Generated from project-owned PNG icons | LVGL image descriptors for provider icons | MIT under this repository | Low | Keep. |
| `firmware/sticks3/assets/wowotou/**` and `firmware/sticks3/generated/vibe_stick_pet_assets.c/.h` | User-authorized Wowotou theme artwork from MiniCPM-Desk-Pet-client / clawd-on-desk | M5Stack firmware home-screen pet frames | User confirmed usage rights for this VibeStick repository; not general-purpose MIT artwork | Medium | Keep source SVGs and generated bitmaps together with NOTICE attribution. Do not reuse outside this repository without checking artwork rights. |
| `firmware/sticks3/generated/vibe_stick_cn_16.c` | Generated from Source Han Sans K Regular | LVGL Chinese glyph subset for StickS3 UI | Source font is SIL Open Font License 1.1, copyright Adobe 2014-2021 | Medium | Keep with NOTICE attribution. Do not use the reserved Source name as an VibeStick brand. |
| `firmware/sticks3/include/vibe_bmi270_config.h` | Derived from M5Unified `BMI270_config.inl` | BMI270 firmware configuration blob for StickS3 lift-to-talk IMU reads | MIT, copyright M5Stack | Low | Keep with source comment. Review upstream before replacing the blob. |
| `firmware/sticks3/src/idf_component.yml` dependencies: `espressif/button`, `espressif/esp_codec_dev`, `lvgl/lvgl` | ESP Component Registry | Build-time firmware dependencies | External open-source components, not vendored after cleanup | Low | Keep dependency manifest and lock file. Review component licenses before binary release. |
| ESP-IDF framework | Espressif | Firmware framework | External SDK, not vendored | Low | Keep as build prerequisite. |
| Groq ASR API | Optional external service | Optional speech-to-text when configured | Service API, no source vendored | Medium | Document that audio leaves the Mac when Groq is configured. Do not commit API keys. |
| Local Codex session files | User-local Codex data | Quota/status observation from `~/.codex/sessions/**/*.jsonl` | User-local data, not vendored | Medium | Keep local-only. Do not upload or commit session data. |
| Historical VoiceStick / StickS3VoiceKit / VoiceStickTrial directories outside this repository | Local historical reference directories in the parent workspace | Not part of VibeStick repository | Source/license uncertain from local copy | High | Do not copy into VibeStick. Do not publish as part of this repository. |
| Old provider logo-like assets removed during cleanup | Earlier local prototype assets | No longer used | Source unclear / brand risk | High | Replaced with simple project-generated temporary icons. |
| `firmware/sticks3/managed_components/`, `firmware/sticks3/build/`, Python `__pycache__/` | Generated local build/cache output | Not part of source | N/A | Low | Ignored by git. Do not commit. |
| `firmware/sticks3/include/vibe_stick_secrets.h`, `.env`, logs, recordings | Local user secrets/output | Runtime configuration and generated data | Private user data | High | Ignored by git. Never publish. |

## Summary

No third-party source code is intentionally vendored in this repository after cleanup. The repository includes the generated Chinese LVGL glyph subset derived from Source Han Sans K under the SIL Open Font License 1.1, the M5Unified BMI270 configuration blob under MIT, and user-authorized Wowotou artwork used only for the firmware home-screen pet. Build-time firmware dependencies are resolved through the ESP-IDF component manager and are not committed as vendored source.

Before a public binary release, review the exact ESP-IDF/component licenses included in the firmware image and ensure the Source Han Sans K and Wowotou attributions remain in NOTICE.
