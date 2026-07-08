# States And Sounds

VibeStick plays short local tones for key agent status changes and recording feedback.

| State | Trigger | Sound |
| --- | --- | --- |
| Completed / 完成 | Codex or Claude reports `DONE`, `COMPLETED`, or `SUCCESS` | 880 Hz 200 ms, 40 ms gap, 1320 Hz 200 ms |
| Error / 报错 | Codex or Claude reports `ERROR`, `FAILED`, or `FAILURE` | 240 Hz 200 ms, 60 ms gap, repeated 3 times |
| Waiting for approval / 等待审批 | Codex or Claude reports `APPROVAL`, `WAITING_APPROVAL`, `PENDING_APPROVAL`, or `NEEDS_APPROVAL` | 600 Hz 200 ms, 60 ms gap, 800 Hz 200 ms |
| Recording start / 开始录音 | Device recording starts successfully | 3600 Hz 90 ms, 18 ms gap, 1800 Hz 90 ms |
| Recording stop / 结束录音 | Device recording stops before upload finalization | 4000 Hz 200 ms |

## No Sound

These states and events do not play sounds:

- Recording in progress.
- Idle.
- Ready.
- Running.
- Thinking.
- Polling.
- Provider switching.
- Quota refresh.
- Quota stale.
- Screen refresh.
- `/state` polling.

## Implementation

Sound generation lives in `firmware/sticks3/src/vibe_audio.c`.

The firmware generates 16 kHz mono 16-bit PCM in memory and plays it through the ES8311 / I2S speaker path. No WAV, MP3, TTS, or network service is used for agent alert sounds.

Recording has priority. If recording is active, alert sounds are skipped instead of queued.

Duplicate prevention lives in `firmware/sticks3/src/main.c`. A sound is played only once for a new `alert.event_id`; if no event id exists, the firmware falls back to status-edge detection.
