# Runtime Refactor Plan

## Goal

Reduce `firmware/sticks3/src/main.c` without changing device behavior,
persisted NVS formats, bridge routing, or OTA compatibility.

## Sequence

1. Keep deterministic policy and data-store rules in host-tested modules.
2. Keep ESP-IDF, NVS, Wi-Fi, socket, LVGL, and task ownership in small runtime
   adapters until their inputs and outputs are explicit.
3. Extract one bounded domain per commit and build both production boards after
   every firmware change.
4. Publish matching OTA images only after a firmware behavior change is ready
   for device validation.

## Current Slice

Recording upload queue draining, task lifecycle, completion signaling, failure
tracking, and diagnostics now live behind `vibe_recording_upload`. Recording
session orchestration, bridge routing, finalization, UI, and Wi-Fi power policy
remain in the composition root.

The earlier slices also established host-tested boundaries for bridge profile
merge rules, Wi-Fi profile policy, OTA decisions, power decisions, follow-up
input policy, WAV validation, and physical button wiring.

## Guardrails

- Preserve per-SSID bridge profile persistence.
- Preserve the selected profile across reboots.
- Do not switch bridge profiles while recording network traffic is active.
- Keep full LAN scans explicit and asynchronous.
