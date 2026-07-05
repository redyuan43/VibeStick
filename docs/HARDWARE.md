# Hardware

## Supported Devices

VibeStick targets M5Stack StickS3 and M5StickC Plus.

## Hardware Used

- Screen: LVGL UI on the built-in 135x240 ST7789 display.
- Front button: push-to-talk recording.
- Side button: short-press provider switching; long-press toggles push-to-talk / lift-to-talk recording.
- IMU: StickS3 uses BMI270 and M5StickC Plus uses MPU6886 for lift-to-talk detection. The device calibrates its flat desktop baseline after boot.
- Microphone: 16 kHz / 16-bit / mono PCM from StickS3 ES8311 or StickC Plus PDM microphone.
- Speaker: StickS3 ES8311 / I2S playback, or StickC Plus GPIO2 tone output.
- Wi-Fi: HTTP communication with the Mac bridge on a 2.4 GHz Wi-Fi network.
- USB: flashing and serial monitor. StickC Plus usually appears as an FT232 `/dev/ttyUSB*` adapter on Linux.
- Battery / USB power: local PMIC reads for the battery UI.

## Firmware Configuration

Install ESP-IDF v5.5.x once before building or flashing firmware:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32,esp32s3
```

Create a local secrets header:

```sh
cp firmware/sticks3/include/vibe_stick_secrets.example.h firmware/sticks3/include/vibe_stick_secrets.h
```

Edit:

```c
#define VIBE_STICK_WIFI_SSID "your-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"
#define VIBE_STICK_BRIDGE_HOST "192.168.1.10"
#define VIBE_STICK_BRIDGE_PORT 8765
#define VIBE_STICK_BRIDGE_TOKEN "paste-generated-token-here"
```

Do not commit `vibe_stick_secrets.h`.

The Wi-Fi network must be 2.4 GHz. If the SSID is a combined 2.4/5 GHz network and the device cannot connect, create or select a dedicated 2.4 GHz SSID.

## Flashing

Load ESP-IDF into every new terminal before running `idf.py`:

```sh
. $HOME/esp/esp-idf/export.sh
```

Adjust the path if ESP-IDF is installed elsewhere. If you see `command not found: idf.py`, this shell has not loaded ESP-IDF yet.

Build or flash through the board-aware wrapper:

```sh
scripts/firmware.sh sticks3 build
scripts/firmware.sh stickc_plus build
scripts/firmware.sh stickc_plus -p /dev/ttyUSB0 flash monitor
```

If automatic flashing fails, put the device into download mode and retry:

1. Plug the device into the computer with a data cable.
2. Use the device-specific download-mode button sequence.
3. Run `ls /dev/cu.*` to find the serial port.
4. Retry `scripts/firmware.sh <board> -p <port> flash`.
5. After flashing, wake the device and confirm the VibeStick home screen appears.

## Runtime Network

The firmware talks to the Mac bridge by HTTP. The Mac bridge should listen on `0.0.0.0:8765` when the device is on the same Wi-Fi network.
