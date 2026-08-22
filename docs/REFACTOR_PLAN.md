# Runtime Refactor Plan

## Goal

Reduce `firmware/sticks3/src/main.c` without changing device behavior,
persisted NVS formats, bridge routing, or OTA compatibility.

## Sequence

1. Keep deterministic policy and data-store rules in host-tested modules.
2. Keep ESP-IDF, NVS, Wi-Fi, socket, LVGL, and task ownership in small runtime
   adapters until their inputs and outputs are explicit.
3. Extract one bounded domain at a time and build all four production targets
   after firmware changes.
4. Publish matching OTA images only after a firmware behavior change is ready
   for device validation.

## Implemented Boundaries

- `main.c` is a minimal entrypoint and contains no JSON, NVS, LVGL, task,
  queue, socket, or HTTP ownership.
- Application/provider/alert state and `/state` parsing are separated into
  `vibe_app_state` and `vibe_state_json`.
- Wi-Fi, bridge registry, bridge HTTP client, recording state, motion state,
  power state, OTA, UI, input routing, and Cardputer coordination each have
  explicit context/config/dependency APIs.
- UI consumes copied view models and owns display/LVGL resources.
- Cardputer input profile, Opt timers, message center, volume, keyboard, and
  air mouse lifecycle are coordinated by `vibe_cardputer_runtime`.
- Pure policy modules are host tested and checked for forbidden ESP-IDF,
  FreeRTOS, LVGL, NVS, driver, and socket imports.
- CI and local checks build `sticks3`, `stickc_plus`, `stickc_plus_se`, and
  `cardputer_adv` independently and compare image sizes with recorded
  baselines.

## Guardrails

- Preserve per-SSID bridge profile persistence.
- Preserve the selected profile across reboots.
- Do not switch bridge profiles while recording network traffic is active.
- Keep full LAN scans explicit and asynchronous.
- Keep every serial firmware flash at `115200` baud.
- Keep board versions and OTA artifacts independent.
- Treat live CapsWriter manifest and real-device verification as release
  gates, not optional post-release checks.
