# Hardware BOM for Costing

This document lists the hardware items relevant to the current VibeStick firmware targets:

- M5Stack StickS3
- M5StickC Plus

Scope: functional BOM for cost accounting, based on the public M5Stack specifications and the components used by this repository's firmware. It does not include every resistor, capacitor, connector contact, PCB test point, adhesive, screw, or internal mechanical subpart, because the public product pages do not provide a manufacturer production BOM with reference designators.

Primary local references:

- `docs/HARDWARE.md`
- `firmware/sticks3/include/vibe_board_profile.h`
- `firmware/sticks3/src/vibe_board.c`
- `firmware/sticks3/src/vibe_audio.c`
- `firmware/sticks3/src/vibe_motion.c`

Primary external references:

- M5Stack StickS3 product documentation: https://docs.m5stack.com/en/core/StickS3
- M5Stack StickC-Plus product documentation: https://docs.m5stack.com/en/core/m5stickc_plus

## StickS3 BOM

| Category | Item / Part | Key configuration | Qty | Firmware use | Notes for costing |
| --- | --- | --- | ---: | --- | --- |
| Main controller | ESP32-S3-PICO-1-N8R8 SiP | Dual-core Xtensa LX7 up to 240 MHz, 2.4 GHz Wi-Fi, integrated 8 MB Flash and 8 MB Octal PSRAM | 1 | Wi-Fi bridge HTTP client, UI, audio, buttons | Treat Flash/PSRAM as included in the SiP unless your supplier prices them separately. |
| Display | ST7789P3 TFT LCD module | 1.14 inch, 135 x 240 color display | 1 | LVGL UI over SPI | Code uses 135 x 240, SPI MOSI/SCK/DC/CS/RST, backlight GPIO38. |
| Audio codec | ES8311 mono audio codec | 24-bit codec, I2S protocol, I2C control at default ES8311 address | 1 | Microphone capture and speaker playback | Used by `VIBE_BOARD_HAS_ES8311`. |
| Microphone | MEMS microphone | SNR 65 dB per M5Stack spec | 1 | 16 kHz, 16-bit, mono voice capture through ES8311 | Exact microphone manufacturer/part is not named in public StickS3 spec. |
| Speaker amplifier | AW8737 power amplifier | Speaker pulse control through M5PM1 PYG3 | 1 | Alert and prompt sounds | Public spec lists AW8737 amplifier. |
| Speaker | 2011 cavity speaker | 8 ohm, 1 W | 1 | Alert and prompt sounds | Public spec lists 8 ohm at 1 W 2011 speaker. |
| IMU | BMI270 6-axis IMU | I2C address 0x68 | 1 | Present in hardware; current VibeStick code does not use StickS3 motion features | Code currently gates motion support to the PDM-mic board path, so this is hardware BOM but not active feature BOM. |
| Power management | M5PM1 PMIC | I2C address 0x6e; battery/VIN ADC, charge state, power hold, L3B, speaker pulse GPIOs | 1 | Battery percentage, USB-powered status, charge state, speaker power control | Public docs expose M5PM1 pin roles; exact IC sourcing may be M5Stack-specific. |
| Battery | Li-ion/LiPo battery | 250 mAh | 1 | Battery status shown locally | Include protection/lead/connector if your costing model separates battery pack from cell. |
| USB connector | USB Type-C connector | DC 5 V input, programming/serial | 1 | Flashing, serial monitor, power | StickS3 uses native ESP32-S3 USB path in firmware configuration. |
| Buttons | Programmable buttons | KEY1 on GPIO11, KEY2 on GPIO12 | 2 | Front push-to-talk and side/power interactions | Include key caps if costing finished device. |
| IR transmitter | IR TX | GPIO46 per public pin map | 1 | Not used by current firmware | Optional for a VibeStick-only derivative if IR is not needed. |
| IR receiver | IR RX | GPIO42 per public pin map | 1 | Not used by current firmware | Public docs warn speaker amplifier interaction with IR receiving. |
| Expansion connector | HY2.0-4P Grove-style connector | GND, 5 V, GPIO9, GPIO10 | 1 | Not used by current firmware | Include if matching M5Stack mechanical compatibility. |
| Expansion connector | Hat2 bus | 2.54 mm, 16 pin | 1 | Not used by current firmware | Include if matching M5Stack ecosystem compatibility. |
| Antenna | 2.4 GHz antenna / RF path | Integrated in StickS3 assembly | 1 | Wi-Fi | Public docs warn against enclosure disassembly because antenna PFC can be damaged. |
| Enclosure | Plastic shell and magnetic back | 48.0 x 24.0 x 15.0 mm product size | 1 | Mechanical | Include magnets, labels, light pipes, and fastening hardware if applicable. |
| PCB and passives | PCB, crystals, passives, regulators around listed ICs | Not fully specified publicly | 1 set | Required platform support | Use official schematic/design files or supplier quotation for exact manufacturing BOM. |

## M5StickC Plus BOM

| Category | Item / Part | Key configuration | Qty | Firmware use | Notes for costing |
| --- | --- | --- | ---: | --- | --- |
| Main controller | ESP32-PICO-D4 SiP | Dual-core processor up to 240 MHz, 2.4 GHz Wi-Fi, 520 KB SRAM, integrated 4 MB Flash | 1 | Wi-Fi bridge HTTP client, UI, audio, buttons, motion mode | Treat 4 MB Flash as included in the SiP unless your supplier prices it separately. |
| Display | ST7789v2 TFT LCD module | 1.14 inch, 135 x 240 color display | 1 | LVGL UI over SPI | Code uses 135 x 240, SPI MOSI/SCK/DC/CS/RST. Backlight is controlled through AXP192 LDO rather than a GPIO. |
| Microphone | SPM1423 PDM microphone | PDM CLK GPIO0, DATA GPIO34 | 1 | 16 kHz, 16-bit, mono voice capture | Used by `VIBE_BOARD_HAS_PDM_MIC`. |
| Speaker / buzzer | Onboard passive buzzer | GPIO2 tone output | 1 | Alert tone output | Current firmware treats this as GPIO tone speaker, not I2S audio playback. |
| IMU | MPU6886 6-axis IMU | I2C address 0x68 | 1 | Lift-to-talk detection | Code checks WHO_AM_I 0x19 and reads accel/gyro samples. |
| RTC | BM8563 RTC | I2C peripheral | 1 | Present in hardware; not used by current firmware | Public StickC-Plus spec lists RTC. |
| Power management | AXP192 PMU | I2C address 0x34; battery/VBUS ADC, charge state, LDO/backlight/output control | 1 | Battery percentage, USB-powered status, charge state, LCD brightness/power setup | Used by non-ES8311 board path. |
| Battery | Li-ion/LiPo battery | 120 mAh at 3.7 V | 1 | Battery status shown locally | Include protection/lead/connector if your costing model separates battery pack from cell. |
| USB bridge / connector | USB Type-C plus USB serial path | Input 5 V at 500 mA; Linux often appears as FT232 `/dev/ttyUSB*` | 1 set | Flashing, serial monitor, power | Public docs mention FTDI driver for USB serial; exact bridge chip should be verified from schematic/supplier. |
| Buttons | Custom buttons | Button A GPIO37, Button B GPIO39 | 2 | Push-to-talk and provider/mode switching | Include key caps if costing finished device. |
| LED | Red LED | GPIO10 per public pin map | 1 | Present in hardware; not used by current firmware | Optional for a VibeStick-only derivative if LED is not needed. |
| IR transmitter | IR TX | GPIO9 per public pin map | 1 | Not used by current firmware | Optional for a VibeStick-only derivative if IR is not needed. |
| Expansion connector | HY2.0-4P Grove-style connector | GND, 5 V, GPIO32, GPIO33 | 1 | Not used by current firmware | Public docs also mention external pins G0, G25/G26, G36, G32, G33. |
| Antenna | 2.4 GHz 3D antenna / RF path | Integrated antenna | 1 | Wi-Fi | Public spec lists 2.4 GHz 3D antenna. |
| Enclosure | Plastic PC shell | 48.0 x 24.0 x 13.5 mm product size | 1 | Mechanical | Include straps/mounting accessories only if bundled in your product. |
| PCB and passives | PCB, crystals, passives, regulators around listed ICs | Not fully specified publicly | 1 set | Required platform support | Use official schematic/design files or supplier quotation for exact manufacturing BOM. |

## Firmware-Relevant Delta

| Area | StickS3 | M5StickC Plus |
| --- | --- | --- |
| MCU family | ESP32-S3-PICO-1-N8R8 | ESP32-PICO-D4 |
| Flash / PSRAM | 8 MB Flash + 8 MB PSRAM | 4 MB Flash, no public PSRAM listing |
| Audio input | MEMS mic through ES8311 | SPM1423 PDM mic |
| Audio output | ES8311 + AW8737 + 8 ohm 1 W speaker | GPIO2 passive buzzer / tone output |
| PMIC | M5PM1 | AXP192 |
| Motion sensor | BMI270 present, not currently active in firmware | MPU6886 used for lift-to-talk |
| Battery | 250 mAh | 120 mAh |
| Expansion | HY2.0-4P + Hat2 bus | HY2.0-4P / external pins |

## Costing Notes

- If the goal is to price complete off-the-shelf M5Stack devices, use the complete device as one line item and optionally keep this BOM as a teardown/feature reference.
- If the goal is to manufacture a compatible derivative, request the official schematic/design files and convert those into a reference-designator BOM before quoting passives, PCB, enclosure, and assembly.
- The current VibeStick feature set does not need IR, RTC, or expansion connectors, but those parts are present on the stock boards and should remain in the BOM when costing the unmodified M5Stack devices.
