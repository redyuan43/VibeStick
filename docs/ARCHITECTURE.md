# VibeStick Architecture

VibeStick has three deliberately separate runtime responsibilities:

1. Device firmware in `firmware/sticks3/`.
2. The CapsWriter M5 bridge in `/home/ivan/github/capswriter-agx-client`.
3. Battery telemetry in this repository's Python package.

Normal devices send state, recording, keyboard, pointer, message, and OTA
traffic to CapsWriter on port `8765`. The Python runtime in this repository is
telemetry-only and listens on port `8878`; it does not receive recordings or
serve OTA firmware.

## Firmware Targets

The production firmware has four independent targets:

| Board | Build target | OTA channel | Notes |
| --- | --- | --- | --- |
| StickS3 | `sticks3` | `sticks3` | ESP32-S3, IMU |
| M5StickC Plus 1.1 | `stickc_plus` | `stickc_plus` | ESP32, IMU |
| M5StickC Plus SE | `stickc_plus_se` | `stickc_plus_se` | ESP32, no IMU path |
| Cardputer Adv | `cardputer_adv` | `cardputer_adv` | Keyboard, Opt, pointer, messages |

Versions, battery curves, build directories, binaries, and OTA manifests are
independent for every target.

## Firmware Layers

`firmware/sticks3/src/main.c` is intentionally tiny. It hands control to the
application composition runtime and does not parse JSON, access NVS, create
tasks, own sockets, or call LVGL.

The firmware is split into these ownership boundaries:

- `vibe_app_state` owns normalized device, provider, and alert state.
- `vibe_state_json` parses `/state`, handles the legacy `codex` block,
  normalizes percentages, and deduplicates TTS requests.
- `vibe_wifi_runtime` owns Wi-Fi profiles, NVS, reconnect timers, connection
  events, RSSI/BSSID snapshots, and Wi-Fi power mode.
- `vibe_bridge_registry` owns per-SSID bridge profiles, target selection,
  persistence, and immutable snapshots.
- `vibe_bridge_client` owns HTTP target snapshots, common headers, response
  capture, and authenticated requests.
- `vibe_recording_controller`, `vibe_motion_controller`,
  `vibe_power_runtime`, and `vibe_ota_runtime` own their state machines and
  expose `init`, `handle`, `tick`, and snapshot-style interfaces.
- `vibe_ui` owns display initialization, LVGL objects, pet animation,
  overlays, settings feedback, bridge-selection feedback, and Cardputer setup
  rendering.
- `vibe_input_router` maps board input signals to application commands.
- `vibe_cardputer_runtime` coordinates the Cardputer keyboard, Opt gesture
  state, input profile, volume, message center, and air mouse lifecycle.
- Board pin and capability differences remain in `vibe_board_profile.h` and
  board adapters.

Pure policy and state modules cannot include ESP-IDF, FreeRTOS, LVGL, NVS,
driver, or socket headers. `scripts/check-firmware-architecture.py` enforces
those boundaries.

## Data And Protocol Compatibility

The refactor does not change:

- HTTP paths, headers, or recording protocol.
- Device IDs or board names.
- NVS namespaces, keys, or stored structure versions.
- Per-SSID bridge selection behavior.
- The rule that recording remains bound to one bridge target.
- The rule that scans do not replace a valid selected target automatically.
- The semantic-version downgrade guard for OTA.

Cross-module reads use copied snapshots. Modules do not expose mutable profile
arrays, locks, timers, tasks, or HTTP clients.

## Build And Release

Run the complete local protection net with:

```sh
./scripts/check.sh --firmware
```

This runs Python tests, firmware host tests, architecture checks, shell checks,
four sequential firmware builds, and OTA partition-size checks. Builds stay
sequential because concurrent ESP-IDF builds can race while resolving managed
components.

Every serial flash is fixed at `115200` baud. A firmware delivery is complete
only when the changed board's matching OTA binary and manifest are published,
the live CapsWriter manifest is verified, and the connected device reports the
new version. Firmware must reject manifests whose semantic version is lower
than the running version, regardless of hash differences.

## Battery Telemetry

Dedicated battery-test firmware posts samples to the Python telemetry service
on port `8878`. These images do not run the normal UI, recording, bridge
selection, or OTA workflow. Their storage and dashboard remain independent
from CapsWriter's production bridge on `8765`.
