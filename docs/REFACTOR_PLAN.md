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

Bridge profile snapshots, identity normalization, ordering, and merge behavior
become a policy boundary. NVS reads and writes, background discovery, and UI
notifications remain in the existing runtime while that boundary is introduced.

## Guardrails

- Preserve per-SSID bridge profile persistence.
- Preserve the selected profile across reboots.
- Do not switch bridge profiles while recording network traffic is active.
- Keep full LAN scans explicit and asynchronous.
