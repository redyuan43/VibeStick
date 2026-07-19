# Battery Telemetry

VibeStick includes dedicated battery-test firmware for:

- M5Stack StickS3 (`ESP32-S3` with `M5PM1`)
- M5StickC Plus 1.1 (`ESP32` with `AXP192`)

These images are test firmware, not replacements for the normal StickS3
VibeStick firmware. They keep the display and Wi-Fi workload stable while
sending power samples to the local bridge.

## What Is Measured

The primary result is battery voltage over elapsed time. Each sample also
records:

- device and board identity
- firmware boot and sequence identity
- battery voltage
- USB/VBUS voltage when available
- charging and USB-powered state
- device uptime and bridge receive time

The percentage shown by the bridge is an estimate derived from voltage. It is
useful for navigation and stop conditions, but it is not a coulomb-counter
capacity measurement.

## Prepare

Run the normal setup first:

```sh
./scripts/setup.sh
```

Telemetry runs independently on port `8878` by default, so it does not share
the normal voice bridge port `8765`. Override it only when that dedicated port
is unavailable:

```sh
VIBE_STICK_TELEMETRY_PORT=8878 ./scripts/setup.sh
VIBE_STICK_TELEMETRY_PORT=8878 ./scripts/battery-test.sh smoke --board stickc_plus_11 --port auto
```

Load ESP-IDF 5.5.x:

```sh
. "$HOME/esp/esp-idf/export.sh"
```

Build both battery-test images:

```sh
./scripts/battery-firmware.sh build all
```

The build helper uses isolated build directories and generated `sdkconfig`
files. It does not trust a developer machine's existing firmware
configuration.

## Smoke Test

Connect one board by USB and run:

```sh
./scripts/battery-test.sh smoke --board sticks3 --port auto
```

For M5StickC Plus 1.1:

```sh
./scripts/battery-test.sh smoke --board stickc_plus_11 --port auto
```

The automation builds and flashes the selected image, waits for fresh powered
telemetry, and then asks for one physical action: unplug the USB cable. The
remaining ten-minute test is automatic. The unplug prompt remains valid for
ten minutes.

A smoke run checks:

- unplugging is confirmed by three consecutive battery-only samples
- at least 80 percent of expected samples arrive
- no sample gap exceeds 20 seconds
- external power does not return during the run
- the ending voltage does not show a sustained implausible rise

## Full Discharge

Run:

```sh
./scripts/battery-test.sh full --board sticks3 --port auto
```

Full mode normally requires external power and at least `4050 mV` before the
unplug step. Use `--allow-partial` only when a partial curve is intentional.

If the device was unplugged before the full session was created, attach the
session to the confirmed unplug point in the current boot's raw journal:

```sh
./scripts/battery-test.sh full DEVICE_ID --resume-unplugged --detach
```

This path does not flash or require USB. It requires fresh Wi-Fi telemetry and
three consecutive battery-only samples, then backfills the session from the
confirmed unplug point.

The test stops when:

- the device stops reporting for two minutes after reaching a low battery
  voltage

The default safety timeout is 12 hours.

StickS3 battery percentage uses a board-specific piecewise-linear discharge
curve calibrated from a fixed always-on white-screen run. StickC Plus 1.1
retains its separate AXP192 voltage mapping and must be calibrated from its own
complete discharge data.

The bridge also appends every accepted telemetry sample to an always-on raw
journal, even when no test session is active. Raw files are grouped by device
and firmware boot ID so a completed discharge cannot be lost when a shorter
session ends.

## Charge Curve

Keep USB connected and start a charge session:

```sh
VIBE_STICK_TELEMETRY_PORT=8878 ./scripts/battery-test.sh charge --board stickc_plus_11
```

The session starts immediately. It completes after three consecutive samples
report USB power, no active charging, and at least `4100 mV`. Charge curves are
useful for charger and full-voltage diagnostics, but should not be used as a
substitute for discharge-based state-of-charge calibration.

## Dashboard And Exports

With the bridge running, open:

```text
http://127.0.0.1:8878/telemetry
```

The dashboard is read-only. It shows current devices and sessions, supports
voltage/percentage views, and can overlay independent StickS3 and Plus 1.1
runs by elapsed time.

Test control remains in the authenticated CLI. Session samples and summaries
are stored below the VibeStick application-support directory and can be
exported as CSV.

Always-on raw telemetry can be exported independently:

```sh
python3 -m vibe_stick.telemetry.cli raw-export DEVICE_ID > raw-telemetry.csv
```

## Interpreting Results

Compare runs made with the same:

- firmware version
- screen profile
- Wi-Fi network and signal conditions
- starting voltage
- room temperature

StickS3 and Plus 1.1 have different processors, PMICs, displays, and battery
capacities. Overlaying their curves is useful, but it is not a controlled
battery-capacity certification.
