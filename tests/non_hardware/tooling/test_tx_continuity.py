"""Synthetic continuity regressions require no SoapySDR bindings or RF hardware."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

try:
    import numpy as np
except ImportError:
    np = None


REPO = Path(__file__).resolve().parents[3]
if np is not None:
    spec = importlib.util.spec_from_file_location(
        "tx_continuity", REPO / "tests/hardware/tx_continuity_validate.py")
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)


@unittest.skipIf(np is None, "NumPy required for synthetic continuity tests")
class ContinuityTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "samples.cs16"
        positions = np.arange(65536)
        samples = 200 * np.exp(2j * np.pi * positions / 8)
        self.iq = np.column_stack((samples.real, samples.imag)).round().astype("<i2")

    def measure(self, **kwargs):
        self.iq.tofile(self.path)
        return validator.measure_signal(self.path, 2e6, 2047, 250000, **kwargs)

    def test_uninterrupted_tone(self):
        result = self.measure()
        self.assertEqual(result["samples"], 65536)
        self.assertEqual(result["longest_low_us"], 0)

    def test_gap_spanning_fft_boundary_is_rejected(self):
        self.iq[16300:16600] = 0
        with self.assertRaisesRegex(AssertionError, "envelope gap"):
            self.measure()

    def test_gap_at_end_is_rejected(self):
        self.iq[-201:] = 0
        with self.assertRaisesRegex(AssertionError, "envelope gap"):
            self.measure()

    def test_partial_final_window_is_checked(self):
        self.iq = self.iq[:33000]
        self.iq[-201:] = 0
        with self.assertRaisesRegex(AssertionError, "envelope gap"):
            self.measure()

    def test_exact_threshold_is_reported_and_allowed(self):
        self.iq[20000:20200] = 0
        self.assertEqual(self.measure()["longest_low_us"], 100)

    def test_short_dip_is_bounded_not_claimed_absent(self):
        self.iq[16380:16400] = 0
        self.assertEqual(self.measure()["longest_low_us"], 10)

    def test_low_level_tone_rejected(self):
        self.iq //= 100
        with self.assertRaisesRegex(AssertionError, "baseline"):
            self.measure()

    def test_noise_is_not_continuous_tone(self):
        rng = np.random.default_rng(247)
        self.iq = np.rint(rng.normal(0, 100, self.iq.shape)).astype("<i2")
        with self.assertRaisesRegex(AssertionError, "expected tone"):
            self.measure()

    def test_wrong_frequency_late_in_capture_rejected(self):
        self.iq[32768:, 1] *= -1
        with self.assertRaisesRegex(AssertionError, "expected tone"):
            self.measure()

    def test_clipping_late_in_capture_rejected(self):
        self.iq[-1] = 2047
        with self.assertRaisesRegex(AssertionError, "clipped"):
            self.measure()

    def test_truncated_iq_rejected(self):
        self.iq.tofile(self.path)
        with self.path.open("ab") as stream:
            stream.write(b"x")
        with self.assertRaisesRegex(AssertionError, "truncated"):
            validator.measure_signal(self.path, 2e6, 2047, 250000)

    def test_invalid_threshold_rejected(self):
        for value in (0, -1, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.measure(max_gap_us=value)

    def test_observer_timestamp_sequence(self):
        previous = validator.timestamp_step(None, 123, 1018, 2e6)
        self.assertEqual(validator.timestamp_step(previous, 509124, 100, 2e6),
                         (509124, 100))

    def test_observer_timestamp_gap_rejected(self):
        for timestamp in (0, 510000, 400000):
            with self.subTest(timestamp=timestamp), self.assertRaises(AssertionError):
                validator.timestamp_step((0, 1018), timestamp, 1018, 2e6)

    def test_empty_observer_read_rejected(self):
        with self.assertRaises(AssertionError):
            validator.timestamp_step(None, 0, 0, 2e6)

    def test_distinct_cards_required(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            validator.parse_args(["--dut-serial", "same", "--observer-serial", "same",
                                  "--output-dir", "unused"])

    def test_finite_parameters_required(self):
        for option in ("--rate", "--deadline", "--dwell", "--retune-hz"):
            with self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit):
                validator.parse_args(["--dut-serial", "dut", "--observer-serial", "peer",
                                      "--output-dir", "unused", option, "nan"])


@unittest.skipIf(np is None, "NumPy required for scripted lifecycle tests")
class ScriptedDutTests(unittest.TestCase):
    def run_dut(self, fail_stop=False):
        import sys
        import time
        from types import SimpleNamespace
        from unittest.mock import patch

        calls = []

        class Device:
            def __init__(self, arguments):
                self.frequency = 1e9
                self.stop_failed = False
                calls.append(("construct", arguments))

            def writeSetting(self, *args):
                pass

            def setSampleRate(self, *args):
                pass

            def setBandwidth(self, *args):
                pass

            def setFrequency(self, direction, channel, frequency):
                calls.append(("frequency", direction, frequency))
                if direction == 0:
                    self.frequency = frequency

            def setGainMode(self, *args):
                pass

            def setGain(self, *args):
                pass

            def setupStream(self, direction, *args):
                return direction

            def getStreamMTU(self, stream):
                return 16384

            def getSampleRate(self, *args):
                return 2e6

            def getNativeStreamFormat(self, *args):
                return SimpleNamespace(fullScale=2047)

            def activateStream(self, stream):
                calls.append(("activate", stream))
                return 0

            def deactivateStream(self, stream):
                calls.append(("stop", stream))
                if stream == 0 and fail_stop and not self.stop_failed:
                    self.stop_failed = True
                    return -2
                return 0

            def writeStream(self, stream, buffers, count, **kwargs):
                time.sleep(0.001)
                return SimpleNamespace(ret=count)

            def readStream(self, stream, buffers, count, **kwargs):
                time.sleep(0.001)
                return SimpleNamespace(ret=count)

            def getFrequency(self, *args):
                return self.frequency

            def closeStream(self, stream):
                calls.append(("close", stream))

        events = []
        messages = iter(("capture", "stop"))
        pipe = SimpleNamespace(send=events.append, poll=lambda seconds: True,
                               recv=lambda: next(messages))
        fake = SimpleNamespace(Device=Device, SOAPY_SDR_RX=0, SOAPY_SDR_TX=1,
                               SOAPY_SDR_CS16="CS16")
        args = validator.parse_args(["--dut-serial", "dut", "--observer-serial", "peer",
                                     "--output-dir", "unused", "--cycles", "2",
                                     "--dwell", "0.01"])
        with patch.dict(sys.modules, SoapySDR=fake):
            if fail_stop:
                with self.assertRaisesRegex(RuntimeError, "RX stop"):
                    validator.dut_worker(args, pipe)
            else:
                validator.dut_worker(args, pipe)
        return calls, events

    def test_same_device_tx_survives_rx_lifecycle_and_submissions_progress(self):
        calls, events = self.run_dut()
        self.assertEqual(sum(c[0] == "construct" for c in calls), 1)
        self.assertEqual(calls.count(("activate", 1)), 1)
        self.assertEqual(calls.count(("stop", 1)), 1)
        self.assertEqual(calls.count(("activate", 0)), 3)
        self.assertEqual(calls.count(("stop", 0)), 3)
        self.assertEqual(events[-1]["phase"], "done")
        for begin, end in zip(events[1::2], events[2:-1:2]):
            self.assertEqual(begin["phase"], "retune_begin")
            self.assertEqual(end["phase"], "retune_end")
            self.assertGreater(end["accepted"], begin["accepted"])
        self.assertLess(calls.index(("stop", 0)),
                        calls.index(("frequency", 0, 1e9 + 100000)))

    def test_failed_rx_stop_does_not_retune_or_restart(self):
        calls, events = self.run_dut(fail_stop=True)
        self.assertEqual(calls.count(("activate", 0)), 1)
        self.assertNotIn(("frequency", 0, 1e9 + 100000), calls)
        self.assertEqual(calls.count(("stop", 1)), 1)
        self.assertEqual({e["phase"] for e in events}, {"ready", "retune_begin"})


@unittest.skipIf(np is None, "NumPy required to import continuity validator")
class EventCoverageTests(unittest.TestCase):
    def events(self):
        return [{"phase": "ready", "observer_samples": 0},
                {"phase": "retune_begin", "cycle": 0, "accepted": 100,
                 "observer_samples": 1000},
                {"phase": "retune_end", "cycle": 0, "accepted": 200,
                 "observer_samples": 2000},
                {"phase": "done", "observer_samples": 3000}]

    def test_complete_interval(self):
        validator.validate_events(self.events(), 1)

    def test_missing_requested_cycle_rejected(self):
        with self.assertRaisesRegex(AssertionError, "incomplete"):
            validator.validate_events(self.events(), 2)

    def test_no_observation_during_retune_rejected(self):
        events = self.events()
        events[2]["observer_samples"] = 1000
        with self.assertRaisesRegex(AssertionError, "does not span"):
            validator.validate_events(events, 1)

    def test_no_tx_submission_progress_rejected(self):
        events = self.events()
        events[2]["accepted"] = 100
        with self.assertRaisesRegex(AssertionError, "TX progress"):
            validator.validate_events(events, 1)


@unittest.skipIf(np is None, "NumPy required for observer orchestration tests")
class ScriptedObserverTests(unittest.TestCase):
    def run_observer(self, fail_read=False, fail_cleanup=False):
        import json
        import sys
        from types import SimpleNamespace
        from unittest.mock import patch

        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        output = Path(directory.name) / "run"
        state = {"blocks": 0, "now": 0.0, "stopped": False}
        events = [(0, {"phase": "ready", "offset": 250000}),
                  (4, {"phase": "retune_begin", "cycle": 0, "accepted": 100}),
                  (8, {"phase": "retune_end", "cycle": 0, "accepted": 200}),
                  (12, {"phase": "done", "accepted": 300})]

        class Pipe:
            def poll(self):
                return bool(events and state["blocks"] >= events[0][0])

            def recv(self):
                return events.pop(0)[1]

            def send(self, command):
                if command == "stop":
                    state["stopped"] = True

        class Process:
            exitcode = 0

            def start(self):
                pass

            def is_alive(self):
                return not state["stopped"]

            def join(self, timeout):
                pass

            def terminate(self):
                state["stopped"] = True

        class Device:
            def __getattr__(self, name):
                if name.startswith("set") or name in ("writeSetting", "closeStream"):
                    return lambda *args: None
                raise AttributeError(name)

            def getSampleRate(self, *args):
                return 2e6

            def getNativeStreamFormat(self, *args):
                return SimpleNamespace(fullScale=2047)

            def getHardwareInfo(self):
                return {"serial": "observer"}

            def setupStream(self, *args):
                return "rx"

            def getStreamMTU(self, stream):
                return 16384

            def activateStream(self, stream):
                return 0

            def deactivateStream(self, stream):
                return -2 if fail_cleanup else 0

            def readStream(self, stream, buffers, count, **kwargs):
                timestamp = state["blocks"] * count * 500
                state["blocks"] += 1
                state["now"] += count / 2e6
                tone = 200 * np.exp(2j * np.pi * np.arange(count) / 8)
                buffers[0][:] = np.column_stack((tone.real, tone.imag)).round().ravel()
                return SimpleNamespace(ret=-1 if fail_read else count,
                                       flags=2, timeNs=timestamp)

        context = SimpleNamespace(Pipe=lambda: (Pipe(), SimpleNamespace(close=lambda: None)),
                                  Process=lambda **kwargs: Process())
        fake = SimpleNamespace(Device=lambda args: Device(), SOAPY_SDR_RX=0,
                               SOAPY_SDR_CS16="CS16", SOAPY_SDR_HAS_TIME=2)
        args = validator.parse_args(["--dut-serial", "dut", "--observer-serial", "peer",
                                     "--output-dir", str(output), "--cycles", "1",
                                     "--dwell", "0.01"])
        with patch.dict(sys.modules, SoapySDR=fake), \
                patch.object(validator.mp, "get_context", return_value=context), \
                patch.object(validator.time, "monotonic", side_effect=lambda: state["now"]), \
                contextlib.redirect_stdout(io.StringIO()):
            if fail_read or fail_cleanup:
                with self.assertRaises(RuntimeError):
                    validator.validate(args)
            else:
                validator.validate(args)
        return json.loads((output / "result.json").read_text())

    def test_observer_captures_complete_interval_and_queued_delivery_tail(self):
        report = self.run_observer()
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["events"]), 4)
        self.assertGreater(report["measurement"]["samples"],
                           report["events"][-1]["observer_samples"])
        self.assertGreaterEqual(report["measurement"]["samples"],
                                report["observer"]["required_samples_through_tail"])

    def test_observer_read_error_records_failure(self):
        self.assertEqual(self.run_observer(fail_read=True)["status"], "FAIL")

    def test_cleanup_error_cannot_leave_pass_report(self):
        report = self.run_observer(fail_cleanup=True)
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("cleanup_error", report)
