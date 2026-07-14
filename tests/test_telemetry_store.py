from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from http import HTTPStatus
from pathlib import Path
from unittest import mock

from vibe_stick.telemetry import store as telemetry_store_module
from vibe_stick.telemetry.store import TelemetryError, TelemetryStore


class TelemetryStoreTests(unittest.TestCase):
    def test_sample_validation_rejects_unknown_and_wrong_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            with self.assertRaises(TelemetryError) as unknown:
                store.submit_sample({**_sample(), "extra": True})
            with self.assertRaises(TelemetryError) as wrong_type:
                store.submit_sample({**_sample(), "usb_powered": "true"})

        self.assertEqual(unknown.exception.status, HTTPStatus.BAD_REQUEST)
        self.assertEqual(wrong_type.exception.status, HTTPStatus.BAD_REQUEST)

    def test_latest_device_registry_updates_and_archives_samples_without_session(self) -> None:
        now = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            store = TelemetryStore(root)
            store.submit_sample(_sample(at=now, battery_mv=4111))
            store.submit_sample(_sample(at=now + timedelta(seconds=5), battery_mv=4108))
            devices = store.list_devices()["devices"]
            raw_logs = list((root / "raw").glob("*/*.jsonl"))
            raw_line_count = len(raw_logs[0].read_text().splitlines())

        self.assertEqual(devices[0]["device_id"], "stick-a")
        self.assertEqual(devices[0]["voltage_mv"], 4108)
        self.assertEqual(devices[0]["board"], "sticks3")
        self.assertEqual(devices[0]["active_session_id"], "")
        self.assertFalse((root / "sessions").exists())
        self.assertEqual(len(raw_logs), 1)
        self.assertEqual(raw_line_count, 2)

    def test_session_requires_fresh_usb_powered_device(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            with self.assertRaises(TelemetryError) as missing:
                store.create_session({"device_id": "stick-a", "kind": "smoke"})
            store.submit_sample(_sample(usb_powered=False, charging=False))
            with self.assertRaises(TelemetryError) as battery_only:
                store.create_session({"device_id": "stick-a", "kind": "smoke"})

        self.assertEqual(missing.exception.status, HTTPStatus.CONFLICT)
        self.assertEqual(battery_only.exception.status, HTTPStatus.CONFLICT)

    def test_three_battery_only_samples_start_discharge_phase_and_persist_files(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            store = TelemetryStore(root)
            store.submit_sample(_sample(at=base))
            session = store.create_session({"device_id": "stick-a", "kind": "smoke"})["session"]

            response = {}
            for index in range(3):
                response = store.submit_sample(
                    _sample(
                        at=base + timedelta(seconds=(index + 1) * 5),
                        usb_powered=False,
                        charging=False,
                    )
                )

            csv_text = store.export_csv(session["id"])
            metadata = json.loads((root / "sessions" / session["id"] / "metadata.json").read_text())
            samples = (root / "sessions" / session["id"] / "samples.jsonl").read_text()

        self.assertEqual(response["session"]["status"], "running")
        self.assertTrue(metadata["unplugged_at"])
        self.assertIn("recorded_at,device_id,boot_id,session_id,battery_percent,voltage_mv", csv_text)
        self.assertEqual(len(samples.splitlines()), 3)

    def test_smoke_session_passes_after_ten_minutes_with_good_cadence(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base, battery_mv=4100))
            store.create_session({"device_id": "stick-a", "kind": "smoke"})
            _confirm_unplug(store, base, battery_mv=4095)

            response = {}
            for index in range(1, 122):
                response = store.submit_sample(
                    _sample(
                        at=base + timedelta(seconds=10 + index * 5),
                        battery_mv=4095 - min(index, 40),
                        usb_powered=False,
                        charging=False,
                    )
                )

        self.assertEqual(response["session"]["status"], "passed")
        self.assertEqual(response["session"]["stop_reason"], "smoke_complete")
        self.assertGreaterEqual(response["session"]["summary"]["coverage"], 0.8)

    def test_smoke_session_fails_when_discharge_samples_have_large_gap(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base))
            store.create_session({"device_id": "stick-a", "kind": "smoke"})
            _confirm_unplug(store, base)
            failed = store.submit_sample(
                _sample(
                    at=base + timedelta(seconds=40),
                    usb_powered=False,
                    charging=False,
                )
            )

        self.assertEqual(failed["session"]["status"], "failed")
        self.assertEqual(failed["session"]["stop_reason"], "max_gap_exceeded")

    def test_external_power_returning_twice_fails_running_session(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base))
            store.create_session({"device_id": "stick-a", "kind": "smoke"})
            _confirm_unplug(store, base)
            store.submit_sample(_sample(at=base + timedelta(seconds=20)))
            failed = store.submit_sample(_sample(at=base + timedelta(seconds=25)))

        self.assertEqual(failed["session"]["status"], "failed")
        self.assertEqual(failed["session"]["stop_reason"], "external_power_returned")

    def test_full_session_enforces_start_voltage_unless_partial_allowed(self) -> None:
        now = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=now, battery_mv=4049))
            with self.assertRaises(TelemetryError) as low:
                store.create_session({"device_id": "stick-a", "kind": "full"})
            partial = store.create_session(
                {
                    "device_id": "stick-a",
                    "kind": "full",
                    "allow_partial": True,
                }
            )

        self.assertEqual(low.exception.status, HTTPStatus.CONFLICT)
        self.assertEqual(partial["session"]["status"], "waiting_for_unplug")

    def test_full_session_can_resume_from_current_boot_raw_unplug_samples(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base, battery_mv=4120))
            for index in range(3):
                store.submit_sample(
                    _sample(
                        at=base + timedelta(seconds=(index + 1) * 5),
                        battery_mv=4115 - index,
                        usb_powered=False,
                        charging=False,
                    )
                )

            session = store.create_session(
                {
                    "device_id": "stick-a",
                    "kind": "full",
                    "resume_unplugged": True,
                }
            )["session"]
            samples = store.list_samples(session["id"])["samples"]

        self.assertEqual(session["status"], "running")
        self.assertTrue(session["started_at"])
        self.assertEqual(session["started_at"], session["unplugged_at"])
        self.assertEqual(session["summary"]["sample_count"], 1)
        self.assertEqual(samples[0]["session_id"], session["id"])
        self.assertFalse(samples[0]["usb_powered"])

    def test_resume_unplugged_is_rejected_without_confirmed_transition(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(usb_powered=False, charging=False))
            with self.assertRaises(TelemetryError) as error:
                store.create_session(
                    {
                        "device_id": "stick-a",
                        "kind": "full",
                        "resume_unplugged": True,
                    }
                )

        self.assertEqual(error.exception.status, HTTPStatus.CONFLICT)

    def test_full_session_waits_for_low_voltage_silence_instead_of_percent(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base, battery_mv=4100))
            session = store.create_session({"device_id": "stick-a", "kind": "full"})["session"]
            _confirm_unplug(store, base, battery_mv=4000)
            response = {}
            for index in range(3):
                response = store.submit_sample(
                    _sample(
                        at=base + timedelta(seconds=20 + index * 5),
                        battery_mv=3335,
                        battery_percent=4,
                        usb_powered=False,
                        charging=False,
                    )
                )
            with mock.patch.object(
                telemetry_store_module,
                "_now",
                return_value=base + timedelta(seconds=160),
            ):
                inferred = store.get_session(session["id"])

        self.assertEqual(response["session"]["status"], "running")
        self.assertEqual(inferred["session"]["status"], "passed")
        self.assertEqual(inferred["session"]["stop_reason"], "full_complete")

    def test_full_session_infers_shutdown_only_after_low_voltage_silence(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base, battery_mv=4100))
            session = store.create_session({"device_id": "stick-a", "kind": "full"})["session"]
            _confirm_unplug(store, base, battery_mv=3390)
            last = base + timedelta(seconds=15)
            with mock.patch.object(
                telemetry_store_module,
                "_now",
                return_value=last + timedelta(seconds=130),
            ):
                inferred = store.get_session(session["id"])

        self.assertEqual(inferred["session"]["status"], "passed")
        self.assertEqual(inferred["session"]["stop_reason"], "full_complete")

    def test_running_session_times_out_after_twelve_hours(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            store = TelemetryStore(root)
            store.submit_sample(_sample(at=base))
            session = store.create_session({"device_id": "stick-a", "kind": "smoke"})["session"]
            metadata_path = root / "sessions" / session["id"] / "metadata.json"
            metadata = json.loads(metadata_path.read_text())
            metadata["created_at"] = (
                datetime.now(timezone.utc) - timedelta(hours=13)
            ).isoformat().replace("+00:00", "Z")
            metadata_path.write_text(json.dumps(metadata))
            response = store.submit_sample(_sample(at=datetime.now(timezone.utc)))

        self.assertEqual(response["session"]["status"], "failed")
        self.assertEqual(response["session"]["stop_reason"], "timeout")

    def test_charge_session_runs_immediately_and_completes_after_stable_full_samples(self) -> None:
        base = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory() as tmp:
            store = TelemetryStore(Path(tmp))
            store.submit_sample(_sample(at=base, battery_mv=3600, charging=True))
            session = store.create_session({"device_id": "stick-a", "kind": "charge"})["session"]
            response = {}
            for index in range(3):
                response = store.submit_sample(
                    _sample(
                        at=base + timedelta(seconds=(index + 1) * 5),
                        battery_mv=4120 + index,
                        usb_powered=True,
                        charging=False,
                    )
                )

        self.assertEqual(session["status"], "running")
        self.assertTrue(session["started_at"])
        self.assertEqual(response["session"]["status"], "passed")
        self.assertEqual(response["session"]["stop_reason"], "charge_complete")


def _confirm_unplug(
    store: TelemetryStore,
    base: datetime,
    *,
    battery_mv: int = 4090,
) -> None:
    for index in range(3):
        store.submit_sample(
            _sample(
                at=base + timedelta(seconds=(index + 1) * 5),
                battery_mv=battery_mv,
                usb_powered=False,
                charging=False,
            )
        )


def _sample(
    *,
    device_id: str = "stick-a",
    at: datetime | None = None,
    battery_percent: int | None = None,
    battery_mv: int | None = 4100,
    usb_powered: bool | None = True,
    charging: bool | None = True,
) -> dict[str, object]:
    payload: dict[str, object] = {
        "schema_version": 1,
        "device_id": device_id,
        "boot_id": "boot-a",
        "board": "sticks3" if device_id == "stick-a" else "stickc_plus_11",
        "pmic": "m5pm1" if device_id == "stick-a" else "axp192",
        "recorded_at": (at or datetime.now(timezone.utc)).isoformat().replace("+00:00", "Z"),
        "battery_mv": battery_mv,
        "battery_percent": battery_percent,
        "usb_mv": 5000 if usb_powered else 0,
        "usb_powered": usb_powered,
        "charging": charging,
        "firmware_name": "vibestick-battery-telemetry",
        "firmware_version": "0.1.0",
        "transport": "HTTP",
        "sequence": 1,
        "uptime_ms": 5000,
        "sample_interval_ms": 5000,
        "wifi_connected": True,
    }
    return payload


if __name__ == "__main__":
    unittest.main()
