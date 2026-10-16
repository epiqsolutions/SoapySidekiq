"""Synthetic RF captures exercise the production detector without radio hardware."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

try:
    import numpy as np
except ImportError:
    np = None


REPO = Path(__file__).resolve().parents[3]
if np is not None:
    # Neither module opens hardware or imports SoapySDR.
    hardware = REPO / "tests/hardware"
    sys.path.insert(0, str(hardware))
    spec = importlib.util.spec_from_file_location(
        "peer_tone_validator", hardware / "full_duplex_pair_validate.py")
    detector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(detector)
    sys.path.pop(0)


@unittest.skipIf(np is None, "NumPy is required for synthetic capture tests")
class PeerToneTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "capture.cs16"

    def capture(self, tones):
        positions = np.arange(32768)
        samples = np.zeros(len(positions), dtype=np.complex128)
        for frequency, amplitude in tones:
            samples += amplitude * np.exp(2j * np.pi * frequency * positions / 2000000)
        np.column_stack((np.rint(samples.real), np.rint(samples.imag))).astype(
            "<i2").tofile(self.path)

    def measure(self, expected=-300000, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return detector.measure_peer_tone(self.path, 2000000, expected, 2047, **kwargs)

    def test_correct_peer_tone(self):
        self.capture([(-300000, 200)])
        self.measure()

    def test_peer_within_frequency_tolerance(self):
        self.capture([(-290000, 200)])
        self.measure()

    def test_wrong_tone_quantization_spur_is_rejected(self):
        self.capture([(100000, 200)])
        with self.assertRaisesRegex(AssertionError, "relative to the strongest"):
            self.measure()

    def test_unrelated_tone_sidelobes_are_rejected(self):
        self.capture([(260000, 200)])
        with self.assertRaises(AssertionError):
            self.measure(expected=220000)

    def test_real_peer_can_be_weaker_than_local_leakage(self):
        self.capture([(100000, 200), (-300000, 20)])
        self.measure()

    def test_peer_below_configured_relative_threshold_is_rejected(self):
        self.capture([(100000, 200), (-300000, 20)])
        with self.assertRaisesRegex(AssertionError, "relative to the strongest"):
            self.measure(min_relative_db=-10)

    def test_silence_is_rejected(self):
        self.capture([])
        with self.assertRaises(AssertionError):
            self.measure()

    def test_noise_without_a_peer_tone_is_rejected(self):
        random = np.random.default_rng(2500)
        for _ in range(30):
            np.rint(random.normal(0, 20, size=(32768, 2))).astype("<i2").tofile(self.path)
            with self.assertRaises(AssertionError):
                self.measure()

    def test_clipped_capture_is_rejected(self):
        self.capture([(-300000, 2047)])
        with self.assertRaisesRegex(AssertionError, "too high|clipped"):
            self.measure()

    def test_invalid_relative_threshold_is_rejected(self):
        for value in (1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                self.measure(min_relative_db=value)
