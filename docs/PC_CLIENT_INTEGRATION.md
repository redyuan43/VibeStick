# PC Client Integration

This document describes what a PC-side client must provide when it replaces or
embeds the default VibeStick bridge. The firmware is intentionally simple: it
talks to one HTTP bridge on the LAN and expects the bridge to own desktop
integration, transcription, paste injection, and OTA file serving.

## Required Bridge Shape

The bridge should listen on a LAN-reachable host and port. The current firmware
defaults to:

```text
http://<pc-ip>:8765
```

If the bridge binds to `0.0.0.0`, use a shared token in production-like setups:

```text
VIBE_STICK_BRIDGE_TOKEN=<shared-token>
```

The firmware sends this token as:

```text
X-Vibe-Stick-Token: <shared-token>
```

Protected endpoints are:

- `POST /event`
- `POST /quota/refresh`
- `POST /recording/start`
- `POST /recording/audio`
- `POST /recording/stop`

OTA `GET` endpoints are intentionally not token-protected in the current
firmware because they are used during recovery-style updates on the local LAN.

## State Endpoint

The firmware polls:

```text
GET /state
```

The response should include the normalized `provider` block documented in
`docs/PROTOCOL.md`. At minimum, return stable values for:

- `active_provider`
- `provider.status`
- `provider.project`
- `provider.quota_5h_remaining`
- `provider.quota_7d_remaining`
- `alert.event_id`
- `alert.type`

Unknown quota values should be `null`, not `0`, so the device can render them as
unknown instead of empty.

## Recording Flow

The firmware records on the M5Stack microphone and uploads raw PCM to the PC
bridge. The PC bridge must not assume the desktop microphone is the source.

Start:

```text
POST /recording/start
Content-Type: application/json
```

Example:

```json
{
  "event": "button_long_start",
  "source": "stickc_plus",
  "audio_source": "stickc_plus_pcm",
  "session_id": "firmware-generated-id"
}
```

Audio chunks:

```text
POST /recording/audio?session_id=<id>&append=1
Content-Type: application/octet-stream
X-Vibe-Stick-Sample-Rate: 16000
X-Vibe-Stick-Channels: 1
X-Vibe-Stick-Bits-Per-Sample: 16
```

Payload format is raw little-endian signed 16-bit mono PCM at 16 kHz.

Stop:

```text
POST /recording/stop
Content-Type: application/json
```

Example:

```json
{
  "event": "button_long_stop",
  "source": "stickc_plus",
  "paste": true
}
```

The bridge should finalize the PCM stream as WAV, run ASR, then paste the result
into the target desktop app if `paste` is true. For Vibe Coding workflows, keep
the target window stable from recording start through paste injection.

Common event names:

- `button_tap_start`
- `button_tap_stop`
- `button_long_start`
- `button_long_stop`
- `motion_lift_start`
- `motion_lift_stop`

## OTA Serving

The firmware checks:

```text
GET /ota/manifest?board=<board>
```

Supported board names:

- `sticks3`
- `stickc_plus`

When no update is available, return:

```json
{
  "available": false,
  "board": "stickc_plus"
}
```

When an update is available, return a manifest shaped like:

```json
{
  "available": true,
  "board": "stickc_plus",
  "version": "v0.1.4-3-gdirty",
  "build_id": "Jul  5 2026 18:17:38 05bab9f8ded0",
  "project_name": "vibe_stick_stickc_plus",
  "idf_version": "v5.5.1",
  "size": 1468000,
  "sha256": "530e96bd300d4e80c146d50a87417c1c88ddd2e7531e82daccbc18267ed8d675",
  "elf_sha256": "05bab9f8ded0d95a8f45b7caa54d2cf5765401c4fab9d94a642d829270f4ac47",
  "file_name": "stickc_plus.bin",
  "url": "/ota/bin?board=stickc_plus"
}
```

The firmware compares `elf_sha256` first. If the current firmware ELF hash
matches the manifest, it skips the update.

The firmware downloads the binary from:

```text
GET /ota/bin?board=<board>
```

The response must be:

```text
HTTP 200
Content-Type: application/octet-stream
Content-Length: <exact binary size>
```

The binary must fit the OTA app partition and `Content-Length` should match
`manifest.size`.

## OTA Artifact Location

The VibeStick repo publishes OTA artifacts into:

```text
firmware/sticks3/ota/
```

Expected files:

```text
stickc_plus.json
stickc_plus.bin
sticks3.json
sticks3.bin
```

For a PC client maintained in a separate repository, either:

- read this directory directly, or
- allow an environment override such as `VIBE_STICK_OTA_DIR`.

The current `capswriter-agx-client` bridge uses:

```text
M5_VOICE_BRIDGE_OTA_DIR
VIBE_STICK_OTA_DIR
../VibeStick/firmware/sticks3/ota
```

in that precedence order.

## Validation Checklist

Before shipping a PC client integration:

- `GET /health` returns `ok: true`.
- `GET /state` returns valid JSON while no device is connected.
- `POST /recording/start` creates or selects one active recording session.
- `POST /recording/audio?...append=1` appends raw PCM chunks.
- `POST /recording/stop` finalizes audio, runs ASR, and paste-injects when
  requested.
- `GET /ota/manifest?board=stickc_plus` returns the newest manifest from
  `firmware/sticks3/ota`.
- `GET /ota/bin?board=stickc_plus` returns the exact binary referenced by the
  manifest.
- The bridge logs OTA manifest and binary requests with remote IP, board, and
  result; this makes field debugging much faster.

## Notes For Porting Existing Voice Clients

When adapting a desktop voice client that normally records from the PC
microphone, split the responsibilities:

- keep hotkey/local-mic recording for the desktop workflow if needed;
- add a separate M5 recording path driven by `/recording/start`,
  `/recording/audio`, and `/recording/stop`;
- treat M5 audio as the authoritative audio source for M5-triggered sessions;
- reuse the existing ASR and paste pipeline after the WAV has been finalized.

This keeps device behavior independent from desktop microphone permissions and
makes it clear whether a transcript came from the M5Stack microphone or the PC
microphone.
