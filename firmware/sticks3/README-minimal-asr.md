# StickS3 maintained ASR runtime

This optional source-built runtime drives the StickS3 microphone, status LCD,
Wi-Fi and CapsWriter bridge. Select `VIBE_STICKS3_MINIMAL_ASR=ON`; it is not a
replacement entry point for the other boards.

## Audio memory policy

| Existing build target | Large ADPCM cache | Fallback queue |
| --- | --- | --- |
| sticks3 | 1 MiB PSRAM, with CONFIG_SPIRAM and sufficient free external heap | 96 frames / 5.76 s |
| cardputer_adv | Disabled; this Stamp-S3A target has no configured PSRAM | 48 frames / 2.88 s |
| stickc_plus | Disabled | 48 frames / 2.88 s |
| stickc_plus_se | Disabled | 48 frames / 2.88 s |
| Unqualified future board | Disabled until explicitly qualified | Existing board default |

Capabilities are selected in `include/vibe_audio_buffer_policy.h`, not from
`CONFIG_IDF_TARGET_ESP32S3`. StickC Plus is the 1.1 model, not Plus2.
Hardware references:
- https://docs.m5stack.com/en/core/StickS3
- https://docs.m5stack.com/en/core/Cardputer-Adv
- https://docs.m5stack.com/en/core/m5stickc_plus

Only the compressed payload storage is explicitly allocated with
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. The FreeRTOS queue control stays internal.
At least 1 MiB external free heap is reserved before allocation; insufficient or
fragmented heap falls back to the previous small internal queue. PCM queues are
unchanged. Transport changes delete the queue and free its external storage.

On ESP32, a 488-byte queue item gives 2148 frames / 128.88 seconds of capacity.
This is queue capacity, not guaranteed offline recovery time. The upload worker
holds the current batch until its session/chunk ACK matches, then reads the next
batch. A failed HTTP transaction retries the identical bytes and chunk ID over
a persistent/reopened connection. Permanent errors are not retried.

The maintained runtime requests an 8000 ms retry budget only with a large queue.
Updated CapsWriter explicitly accepts it per StickS3 protocol-v2 upload session;
legacy clients keep the 5000 ms default budget. The bridge grants 5 s additional
watchdog margin. The ASR relay currently has a 15 s input idle timeout and a
10 s pending audio limit: multi-minute disconnected recording/replay is NOT
implemented. No Flash/NVS audio writes or offline session recovery are added.

The serial summary includes queue peak, dropped frames, retry count and final
result. USB serial byte `r` toggles recording and follows the same path as the
front button.

## Build and delivery

Create the ignored private `include/vibe_sticks3_asr_local.h` with:

```c
#pragma once
#define STICKS3_ASR_BRIDGE_HOST "YOUR_LAN_HOST"
#define STICKS3_ASR_BRIDGE_TOKEN "YOUR_BRIDGE_TOKEN"
```

Wi-Fi credentials remain in device NVS. Never add this header or credential-
containing binaries to Git.

From an ESP-IDF 5.5.1 environment:

```sh
idf.py -C "firmware/sticks3" -B "build-sticks3-asr-stable" \
  -D VIBE_BOARD=sticks3 -D VIBE_STICKS3_MINIMAL_ASR=ON \
  -D SDKCONFIG="$PWD/build-sticks3-asr-stable/sdkconfig" build
```

Flash at **115200 baud**. Preserve device NVS and the working recovery image.
Publish the same StickS3 binary and its matching OTA manifest, verify the live
manifest/image hashes, and verify device boot version before delivery.
Other boards' binaries and OTA manifests are independent.

## Validation on 2026-09-19

- Source build: ESP-IDF 5.5.1, StickS3 minimal runtime 0.1.81.
- Device boot: 8 MiB PSRAM detected; 1,048,224 bytes usable queue storage,
  2148 frames, 128880 ms capacity; 7,336,432 bytes external heap left.
- Host allocator harness executes the production allocation function across
  five board defines and PSRAM enabled/disabled, including low-memory,
  allocation-failure and static-queue-failure fallback cases.
- CapsWriter: 226 tests passed, including per-board retry negotiation, duplicate
  start/chunk handling and ended-session invalidation.
- Physical 20-second recording: pause only the desktop ingress process for
  6.5 seconds, then resume it. Peak backlog 116 frames (6.96 seconds), exceeding
  the old 96-frame capacity; 3 retries, 0 dropped frames, 645120 PCM bytes across
  84 accepted chunks, final status pasted. This simulates a stalled receiver,
  not an access-point disconnection.
- A preceding automatic speaker test uploaded all frames but returned an empty
  ASR final. That run is not counted as end-to-end success. Playback was then
  explicitly targeted to the USB speaker; the fault-injection run above passed.
- Only StickS3 OTA was published; live manifest and downloaded binary hashes
  match the flashed source-built artifact. Other boards were not physically
  exercised or reflashed.

- Physical 60-second recording with the same 6.5-second ingress pause: peak 110
  frames, 3 retries, 0 dropped frames; 1925760 PCM bytes across 251 accepted
  chunks; final status pasted. Both injected-stall runs passed.
