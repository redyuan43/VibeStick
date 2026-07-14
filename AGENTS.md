# Project Instructions

## Serial Flashing Rule

- For every serial firmware flash, use `115200` baud for both StickS3 and
  M5StickC Plus 1.1. Do not use a higher baud rate for any board.

## Firmware And OTA Release Invariant

- A firmware version or binary change is not complete until the matching OTA
  image and manifest are published in the same delivery.
- Publish only the changed board target. StickS3 and StickC Plus firmware,
  battery curves, binaries, and OTA manifests must remain independent.
- After publishing, query `/ota/manifest?board=<board>` from the live OTA
  service and verify its version, build ID, size, and hashes match the newly
  built image.
- Verify the connected device reports the new firmware version after flashing
  or OTA installation.
- Device OTA logic must reject manifests whose semantic version is lower than
  the running firmware version. A different hash alone must never authorize a
  downgrade.

## Bridge Port Convention

- The M5 bridge in `/home/ivan/github/capswriter-agx-client` uses the fixed
  port `8765` and is the default receiver for normal device use.
- This repository's `scripts/dev.sh` is only for standalone Python bridge
  debugging and defaults to port `8766` to avoid conflicting with CapsWriter.
- Start only CapsWriter for normal device use. Do not run both bridges at the
  same time, and do not change `scripts/dev.sh` back to port `8765`.
- Firmware still targets CapsWriter at `192.168.31.225:8765`. Tests using
  `scripts/dev.sh` must explicitly configure a matching target and must not
  assume devices will automatically connect to port `8766`.
