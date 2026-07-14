# Project Instructions

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
