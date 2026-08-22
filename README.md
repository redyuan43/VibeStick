# VibeStick

[中文说明](README.zh-CN.md)

![VibeStick home screen showing Codex and Claude providers](assets/brand/home-screen-preview.png)

VibeStick turns an M5Stack StickS3, M5StickC Plus, M5StickC Plus SE, or
Cardputer Adv into a compact wireless voice-input controller. The production
bridge, OTA service, device registry, and desktop interaction run in
CapsWriter on port `8765`.

The Python package in this repository is intentionally telemetry-only. It
stores and serves battery-test data on port `8878`; it is not a second voice
bridge.

VibeStick targets M5Stack StickS3, M5StickC Plus, M5StickC Plus SE, and
Cardputer Adv hardware and is not an official M5Stack project. Third-party
agent names such as Codex and Claude describe compatible local tools and
integrations only.

## Battery discharge telemetry

The repository also contains dedicated battery-test firmware for StickS3 and
M5StickC Plus 1.1. These images keep the screen and Wi-Fi workload stable,
report battery voltage to the bridge over Wi-Fi, and continue working after
the USB cable is removed.

They are test images, not full VibeStick ports. The Plus 1.1 image does not
include the agent UI, recording, or alert sounds.

Typical smoke tests:

```sh
./scripts/battery-test.sh smoke --board sticks3 --port auto
./scripts/battery-test.sh smoke --board stickc_plus_11 --port auto
```

Open the read-only live dashboard at:

```text
http://127.0.0.1:8878/telemetry
```

See [Battery Telemetry](docs/BATTERY_TELEMETRY.md) for isolated builds,
flashing, full-discharge runs, exports, and result interpretation.

## What you'll need (prepare first)

- [ ] M5Stack StickS3 or M5StickC Plus and a data cable.
- [ ] A computer running the CapsWriter M5 bridge on the same network.
- [ ] Wi-Fi name and password. The Wi-Fi must be 2.4 GHz; StickS3 / ESP32-S3 does not support 5 GHz Wi-Fi.
- [ ] CapsWriter configured with the desired ASR backend.

Building the firmware needs ESP-IDF v5.5.x — a one-time toolchain install (~1 GB, a few minutes). The install steps below set it up for you; no need to pre-install. Reference: Espressif's [ESP-IDF v5.5.1 ESP32-S3 guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html).

## Install

You can do this manually, or hand the command steps to an AI coding agent such as Claude Code and Codex.

> Legend: steps marked 👤 are PHYSICAL steps that need a human to act directly, such as plugging in the cable, long-pressing or short-pressing the power button, and granting macOS permissions in System Settings. AI agents should run the shell steps in order, then pause at each 👤 step and ask the user to complete it before continuing.

1. Clone the repo and create local config files:

```sh
git clone https://github.com/GaryGaryyy/VibeStick.git
cd VibeStick
./scripts/setup.sh
```

2. Fill the local config values the human prepared:

```sh
open -e firmware/sticks3/include/vibe_stick_secrets.h
```

In `vibe_stick_secrets.h`, set Wi-Fi SSID, Wi-Fi password, and the Mac bridge host. `scripts/setup.sh` tries to auto-fill `VIBE_STICK_BRIDGE_HOST` with the detected en0 LAN IP when the file still has the example placeholder. Add `VIBE_STICK_WIFI_PROFILES` there if the device should remember multiple 2.4 GHz networks; the firmware stores those profiles in ESP NVS so normal OTA updates keep them.

Configure ASR, provider observation, and paste behavior in CapsWriter, not in
this repository. This repository's `.env` is only for the telemetry token,
port, and URL.

3. 👤 Plug the StickS3 into the Mac with the USB-C data cable.

4. 👤 Put the StickS3 into download mode: long-press the side power button until the blue LED double-blinks and the screen turns off. This is required for ESP32-S3 flashing.

5. Install ESP-IDF if it is not already present, then load it into the current shell. This is a one-time toolchain install with a large ~1 GB download and can take a few minutes. Run the load command in every new terminal before `idf.py`:

```sh
if [ ! -d "$HOME/esp/esp-idf" ]; then
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32,esp32s3
fi
. "$HOME/esp/esp-idf/export.sh"
```

Or install via Espressif's [official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html). If `install.sh` fails, ensure `git`, `python3`, and `cmake` are present, or follow the official guide. Adjust the path if ESP-IDF is installed elsewhere.

6. Build and flash the firmware:

```sh
./scripts/firmware.sh stickc_plus -p <port> -b 115200 build flash

# M5StickC Plus SE (no IMU or lift-to-talk support)
./scripts/firmware.sh stickc_plus_se -p <port> -b 115200 build flash

# Cardputer Adv
./scripts/firmware.sh cardputer_adv -p <port> -b 115200 build flash
```

For StickS3, replace `stickc_plus` with `sticks3`. The script rejects serial
flash baud rates other than `115200`.

If you do not know the port, run:

```sh
ls /dev/cu.*
```

Wait for `Hash of data verified`.

7. 👤 Short-press the power button to wake the screen. The blue LED should turn off, the screen should turn on, and you should see the VibeStick home screen. Before networking is ready, it may show offline.

8. Start the CapsWriter M5 bridge from
`/home/ivan/github/capswriter-agx-client` and verify it listens on `8765`.
Follow that project's installation instructions for the macOS app, ASR, and
paste permissions.

9. 👤 Grant Accessibility permission to the CapsWriter application when macOS
prompts. This permission is needed for paste injection.

10. Check the setup:

```sh
./scripts/doctor.sh
```

Aim for all firmware and telemetry checks to pass. CapsWriter has its own
runtime diagnostics for the production bridge.

11. 👤 Open any text box, press the front blue button once, speak, and press it again to send. Long-press and release still works as push-to-talk. In both modes, VibeStick transcribes audio from the device microphone and pastes the text automatically.

StickS3 and M5StickC Plus also support lift-to-talk mode. The default `PTT` mode supports front-button tap-to-talk and push-to-talk behavior. On StickS3, hold the side button for 3 seconds to open settings; a single side-button click does nothing, while a quick double-click starts bridge discovery. Inside settings, a short side-button press cycles through `MODE`, `SLEEP`, and `VERSION`; a short front-button press changes the selected value; holding the front button for 1.5 seconds saves the settings. `MODE` selects `PTT` or `LIFT`, and `SLEEP` selects 1, 2, 5, or 10 minutes (5 minutes by default). In `LIFT`, the device uses its flat desktop pose as the baseline, starts recording when lifted, and sends recognition after it is placed back flat and stable. The onboard status LED lights while the front button is held or a recording is active, and is forced off before deep sleep.

`./scripts/dev.sh` in this repository starts only the battery telemetry service
on `8878`; it cannot replace CapsWriter for normal device use.

## Wi-Fi OTA Firmware Updates

The firmware now uses dual OTA app partitions. The first upgrade from the old single-app partition layout still needs one full USB flash. After that one-time flash, future firmware builds can be published through the bridge on the same Wi-Fi network.

Build and publish an OTA image for a board:

```sh
. "$HOME/esp/esp-idf/export.sh"
./scripts/firmware.sh sticks3 build
./scripts/ota_publish.sh sticks3
```

Repeat the build and publish commands with `stickc_plus`, `stickc_plus_se`, or
`cardputer_adv` for the other independent targets. Published files are written
to `firmware/sticks3/ota/`; CapsWriter serves them through
`/ota/manifest?board=...` and `/ota/bin?board=...`. Once the device is on
Wi-Fi, it installs an image only when the manifest has a strictly higher
semantic version than the running firmware. Equal or lower versions are
rejected even when their build IDs or hashes differ. Accepted images are
downloaded to the inactive OTA slot before the device switches boot
partitions and restarts.

## Troubleshooting

### `command not found: idf.py`

ESP-IDF is installed but not loaded into the current shell, or it has not been installed yet. Source ESP-IDF's `export.sh`, then run `idf.py` again:

```sh
. $HOME/esp/esp-idf/export.sh
```

Adjust the path if your ESP-IDF checkout is somewhere else. Run this once in every new terminal before using `idf.py`.

### Flashing says "Device not configured" or cannot open the serial port

Unplug and replug the USB-C data cable. Put the StickS3 into download mode
again: long-press the side power button until the blue LED double-blinks and
the screen turns off. Run `ls /dev/cu.*` to find the port, then retry through
the board wrapper so flashing remains fixed at `115200` baud:

```sh
./scripts/firmware.sh <board> -p <port> -b 115200 build flash
```

### StickS3 cannot join Wi-Fi

Use a 2.4 GHz Wi-Fi network. StickS3 / ESP32-S3 does not support 5 GHz Wi-Fi.

### Recording transcribes but does not paste

Check CapsWriter's Accessibility permission and paste diagnostics. The Python
telemetry process in this repository never performs paste injection.

### "No transcription adapter configured"

Configure the ASR backend in CapsWriter. This repository has no transcription
adapter.

### Cannot find `.env`

`.env` is a hidden file used only by local telemetry scripts. Open it with:

```sh
open -e .env
```

### Transcription fails or times out with SSL/network errors

Use CapsWriter's ASR diagnostics and configuration. VibeStick firmware only
uploads PCM to CapsWriter on `8765`.

## Configuration Ownership

Do not commit real API keys, local tokens, Wi-Fi credentials, local logs, or generated recording files.

- `firmware/sticks3/include/vibe_stick_secrets.h`: device Wi-Fi,
  `192.168.31.225:8765` CapsWriter target, bridge profiles, and device token.
- `.env`: local battery telemetry settings such as
  `VIBE_STICK_TELEMETRY_TOKEN`, `VIBE_STICK_TELEMETRY_PORT`, and
  `VIBE_STICK_TELEMETRY_URL`.
- CapsWriter configuration: ASR, provider status, quota observation, device
  registry, recordings, OTA serving, and paste behavior.

## Project layout

```text
VibeStick/
  README.md
  README.zh-CN.md
  .env.example
  docs/
  firmware/sticks3/
  firmware/telemetry/
  bridge/src/vibe_stick/
  app/macos/VibeStickHUD/
  scripts/
  tests/
```

## Checks

```sh
./scripts/check.sh
```

Firmware builds still require ESP-IDF:

```sh
. $HOME/esp/esp-idf/export.sh
./scripts/check.sh --firmware
```

## Current limits

- Real-device regression, serial flashing, and live OTA verification require
  connected hardware and a running CapsWriter service.
- Cardputer has the smallest OTA margin and should be watched during future
  feature growth.

## Contributing & Security

Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). To report a vulnerability,
see [SECURITY.md](SECURITY.md) (please report privately).

## License

VibeStick is released under the MIT License. See [LICENSE](LICENSE).
