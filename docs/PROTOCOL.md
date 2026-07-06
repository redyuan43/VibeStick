# Protocol

VibeStick v0.1.2 uses HTTP over Wi-Fi between the device firmware and the local Mac bridge.

Default bridge URL:

```text
http://<mac-ip>:8765
```

## Firmware Headers

Firmware requests include:

```text
X-Vibe-Stick-Firmware-Name: vibestick
X-Vibe-Stick-Firmware-Version: 0.1.2
X-Vibe-Stick-Firmware-Transport: HTTP
X-Vibe-Stick-Firmware-Build-Date: <compile date>
```

Audio upload requests additionally include:

```text
X-Vibe-Stick-Sample-Rate: 16000
X-Vibe-Stick-Channels: 1
X-Vibe-Stick-Bits-Per-Sample: 16
```

Low-memory firmware targets may upload audio in append chunks:

```text
POST /recording/audio?session_id=<id>&append=1
Content-Type: application/octet-stream
```

The bridge appends those chunks to a temporary PCM file and finalizes it as WAV when `/recording/stop` arrives. A single full-body upload without `append=1` remains supported.

When `VIBE_STICK_BRIDGE_TOKEN` is configured on the bridge and firmware, protected POST requests also include:

```text
X-Vibe-Stick-Token: <shared-token>
```

Protected endpoints are `/event`, `/quota/refresh`, `/recording/start`, `/recording/audio`, and `/recording/stop`. If the bridge binds outside loopback, such as `0.0.0.0`, `VIBE_STICK_BRIDGE_TOKEN` is required and placeholder tokens are rejected. If the bridge binds to loopback only, missing tokens are allowed for local development.

## GET /state

Returns the current bridge state:

```json
{
  "time": "13:01",
  "wifi": true,
  "ble": false,
  "battery": null,
  "active_provider": "claude",
  "provider": {
    "id": "claude",
    "display_name": "Claude",
    "implemented": true,
    "status": "RUNNING",
    "project": "vibestick",
    "quota_5h_remaining": 66,
    "quota_7d_remaining": 96,
    "quota_updated_at": "13:01",
    "quota_stale": false
  },
  "codex": {
    "status": "RUNNING",
    "project": "vibestick",
    "quota_5h_remaining": 53,
    "quota_7d_remaining": 93,
    "quota_updated_at": "13:01",
    "quota_stale": false
  },
  "alert": {
    "event_id": "",
    "type": "NONE",
    "message": ""
  },
  "bridge_name": "vibestick-bridge",
  "bridge_version": "0.1.2"
}
```

`battery` is intentionally `null` from the bridge. The StickS3 displays its local PMIC battery reading.

`active_provider` selects which normalized `provider` block the firmware should render. `provider.quota_5h_remaining` and `provider.quota_7d_remaining` are remaining percentages from `0` to `100`; `null` means unknown and the firmware renders `--%`. The legacy `codex` block remains present for backward compatibility.

## GET /health

Returns bridge health metadata:

```json
{
  "ok": true,
  "bridge_name": "vibestick-bridge",
  "bridge_version": "0.1.2"
}
```

## GET /ota/manifest

Returns the latest firmware update manifest for the requested board:

```text
GET /ota/manifest?board=stickc_plus
```

Supported board names are `sticks3` and `stickc_plus`.

No update:

```json
{
  "available": false,
  "board": "stickc_plus"
}
```

Update available:

```json
{
  "available": true,
  "board": "stickc_plus",
  "version": "v0.1.4-3-gdirty",
  "build_id": "Jul  5 2026 18:17:38 05bab9f8ded0",
  "size": 1468000,
  "sha256": "<bin-sha256>",
  "elf_sha256": "<elf-sha256>",
  "file_name": "stickc_plus.bin",
  "url": "/ota/bin?board=stickc_plus"
}
```

The firmware compares `elf_sha256` first. If it matches the currently running
app, the device skips the update.

## GET /ota/bin

Returns the OTA firmware binary referenced by the manifest:

```text
GET /ota/bin?board=stickc_plus
```

The response body is `application/octet-stream`, and `Content-Length` should
match the manifest `size`.

See `docs/PC_CLIENT_INTEGRATION.md` for the PC-client integration checklist and
OTA artifact layout.

## POST /event

Receives generic firmware or debug events.

Examples:

```json
{"event":"test_agent_status","source":"manual_test","status":"DONE","message":"test done"}
```

Manual `DONE`, `ERROR`, and `APPROVAL` statuses produce alert fields for local testing.

## POST /quota/refresh

Requests a quota refresh for the active provider. Codex refreshes from local session events. Claude refreshes the cached usage snapshot only when `VIBE_STICK_CLAUDE_USAGE` is enabled; failures keep the provider quota fields `null` so the firmware shows `--%`.

```json
{
  "refreshed": true,
  "state": {
    "time": "13:01",
    "wifi": true,
    "battery": null
  }
}
```

## POST /recording/start

Starts a recording session:

```json
{
  "event": "button_tap_start",
  "source": "sticks3",
  "audio_source": "sticks3_pcm",
  "session_id": "<firmware-generated-id>"
}
```

Push-to-talk uses `button_long_start`; lift-to-talk uses `motion_lift_start`.

## POST /recording/audio

Uploads raw little-endian signed PCM for the active session:

```text
POST /recording/audio?session_id=<id>
Content-Type: application/octet-stream
```

The bridge writes a local WAV file under:

```text
~/Library/Application Support/VibeStick/Recordings/
```

The bridge rejects audio uploads larger than `VIBE_STICK_MAX_RECORDING_AUDIO_BYTES`. The default is `2000000` bytes.

## POST /recording/stop

Stops the session and runs transcription:

```json
{"event":"button_tap_stop","source":"sticks3","paste":true}
```

Push-to-talk uses `button_long_stop`; lift-to-talk uses `motion_lift_stop`.

When transcription succeeds, the bridge pastes the transcript into the focused macOS app. Recording status does not trigger agent alert sounds.
