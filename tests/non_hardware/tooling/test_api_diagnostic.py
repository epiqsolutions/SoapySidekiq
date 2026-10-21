"""Exercise the hardware API diagnostic without bindings, cards, or RF equipment."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


SCRIPT = Path(__file__).resolve().parents[2] / "hardware" / "test_api.py"
spec = importlib.util.spec_from_file_location("sidekiq_api_diagnostic", SCRIPT)
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def span(low, high, step=0):
    return SimpleNamespace(minimum=lambda: low, maximum=lambda: high, step=lambda: step)


class FakeDevice:
    """Fail on unexpected calls; all supported operations are getter-only."""
    def __init__(self):
        self.calls = []
        self.failures = set()
        self.counts = {1: 2, 0: 1}
        self.native = SimpleNamespace(format="CS16", fullScale=32767)
        self.gain_range = span(0, 76, 1)

    def __getattr__(self, name):
        operations = {
            "getDriverKey": lambda: "Sidekiq",
            "getHardwareKey": lambda: "test card",
            "getHardwareInfo": lambda: {"card": "5", "serial": "TEST"},
            "getNumChannels": lambda direction: self.counts[direction],
            "getStreamFormats": lambda *_: ["CS16", "CF32"],
            "getNativeStreamFormat": lambda *_: self.native,
            "getStreamArgsInfo": lambda *_: [],
            "listAntennas": lambda *_: [],
            "listGains": lambda *_: ["LNA"],
            "getGainRange": lambda *_: self.gain_range,
            "hasGainMode": lambda direction, _: direction == 1,
            "getFrequencyRange": lambda *_: [span(70000000, 6000000000)],
            "getSampleRateRange": lambda *_: [span(625000, 61440000)],
            "getFrequency": lambda *_: 100000000,
            "getSampleRate": lambda *_: 5000000,
            "getBandwidth": lambda *_: 4000000,
            "listSensors": lambda: ["temperature", "accelerometer"],
            "getSensorInfo": lambda key: SimpleNamespace(
                key=key, name=key, description="sensor", units="C", type=1),
            "readSensor": lambda key: "42" if key == "temperature" else "{}",
            "listClockSources": lambda: ["internal_clock"],
            "listTimeSources": lambda: ["1pps_source_unavailable"],
        }
        if name not in operations:
            raise AssertionError(f"unexpected hardware operation: {name}")

        def call(*args):
            self.calls.append((name, args))
            if name in self.failures:
                raise RuntimeError(f"injected {name} failure")
            return operations[name](*args)
        return call


class ApiDiagnosticTests(unittest.TestCase):
    def test_default_checks_all_advertised_channels_without_writes_or_sensor_reads(self):
        device = FakeDevice()
        result = api.collect_checks(device, [("RX", 1), ("TX", 0)])
        self.assertEqual(result["summary"]["fail"], 0)
        self.assertEqual([args for name, args in device.calls if name == "getSampleRate"],
                         [(1, 0), (1, 1), (0, 0)])
        self.assertFalse(any(name == "readSensor" for name, _ in device.calls))
        # FakeDevice rejects every setter and stream operation, so an unexpected
        # side effect becomes a reported failure rather than silently passing.
        self.assertTrue(all(item["status"] == "PASS" for item in result["checks"]))

    def test_invalid_selected_channel_is_reported_without_channel_api_access(self):
        device = FakeDevice()
        result = api.collect_checks(device, [("RX", 1)], [2])
        self.assertEqual(result["summary"]["fail"], 1)
        self.assertFalse(any(name == "getStreamFormats" for name, _ in device.calls))
        self.assertTrue(any(item["name"] == "clock sources" for item in result["checks"]))

    def test_failure_does_not_hide_other_channel_or_sensor_checks(self):
        device = FakeDevice()
        device.failures.add("getFrequency")
        result = api.collect_checks(device, [("RX", 1)])
        self.assertEqual(result["summary"]["fail"], 2)
        self.assertTrue(any(item["name"] == "sensor temperature metadata" and
                            item["status"] == "PASS" for item in result["checks"]))
        errors = [item["error"] for item in result["checks"] if item["status"] == "FAIL"]
        self.assertTrue(all("getFrequency" in error for error in errors))

    def test_absent_direction_is_skipped_but_explicit_missing_channel_fails(self):
        device = FakeDevice()
        device.counts[0] = 0
        default = api.collect_checks(device, [("TX", 0)])
        selected = api.collect_checks(device, [("TX", 0)], [0])
        self.assertEqual(default["summary"]["skip"], 1)
        self.assertEqual(default["summary"]["fail"], 0)
        self.assertEqual(selected["summary"]["fail"], 1)

    def test_inconsistent_native_metadata_and_invalid_ranges_are_failures(self):
        device = FakeDevice()
        device.native = SimpleNamespace(format="UNKNOWN", fullScale=float("nan"))
        device.gain_range = span(10, 0)
        result = api.collect_checks(device, [("TX", 0)])
        self.assertEqual(result["summary"]["fail"], 2)
        self.assertTrue(any(item["name"] == "TX channel 0 RF readback" and
                            item["status"] == "PASS" for item in result["checks"]))

    def test_sensor_reads_are_explicit_and_keep_raw_optional_sensor_values(self):
        device = FakeDevice()
        result = api.collect_checks(device, [], read_sensors=True)
        self.assertEqual(result["summary"]["fail"], 0)
        self.assertEqual([args for name, args in device.calls if name == "readSensor"],
                         [("temperature",), ("accelerometer",)])
        self.assertEqual(next(item["details"] for item in result["checks"]
                              if item["name"] == "sensor accelerometer readback"), "{}")

    def test_cli_json_and_exit_status_record_success_and_independent_failures(self):
        for failed in (False, True):
            with self.subTest(failed=failed), tempfile.TemporaryDirectory() as temporary:
                device = FakeDevice()
                if failed:
                    device.failures.add("getHardwareInfo")
                factory = Mock(return_value=device)
                bindings = SimpleNamespace(Device=factory, SOAPY_SDR_RX=1, SOAPY_SDR_TX=0)
                output = Path(temporary) / "result.json"
                with patch.dict(sys.modules, {"SoapySDR": bindings}), contextlib.redirect_stdout(io.StringIO()):
                    status = api.main(["--serial", "TEST", "--direction", "rx", "--channel", "1",
                                       "--json", str(output)])
                report = json.loads(output.read_text())
                factory.assert_called_once_with({"driver": "sidekiq", "serial": "TEST"})
                self.assertEqual(status, int(failed))
                self.assertEqual(report["summary"]["fail"], int(failed))
                self.assertEqual(report["device_args"]["serial"], "TEST")
                self.assertEqual([args for name, args in device.calls if name == "getSampleRate"], [(1, 1)])

    def test_failed_device_construction_still_writes_failure_report(self):
        bindings = SimpleNamespace(Device=Mock(side_effect=RuntimeError("card busy")))
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "result.json"
            with patch.dict(sys.modules, {"SoapySDR": bindings}), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(api.main(["-c", "5", "--json", str(output)]), 1)
            report = json.loads(output.read_text())
            self.assertEqual(report["summary"]["fail"], 1)
            self.assertIn("card busy", report["checks"][0]["error"])

    def test_selector_and_numeric_errors_fail_before_loading_bindings(self):
        for args in ([], ["-c", "-1"], ["-c", "256"], ["--serial", " "],
                     ["-c", "0", "--serial", "TEST"], ["-c", "0", "--channel", "-1"],
                     ["-c", "0", "--topology", "256"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    api.parse_args(args)
                self.assertEqual(caught.exception.code, 2)

    def test_help_does_not_import_hardware_bindings(self):
        with tempfile.TemporaryDirectory() as temporary:
            Path(temporary, "SoapySDR.py").write_text("raise RuntimeError('bindings imported')\n")
            env = dict(os.environ, PYTHONPATH=temporary, PYTHONDONTWRITEBYTECODE="1")
            result = subprocess.run([sys.executable, str(SCRIPT), "--help"], env=env,
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--read-sensors", result.stdout)


if __name__ == "__main__":
    unittest.main()
