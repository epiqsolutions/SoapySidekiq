#!/usr/bin/env python3

"""Run two separate-card full-duplex examples and measure both captures."""

import argparse
import math
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

from paired_tone_validate import REPO, wait_for_marker


def measure_peer_tone(path, rate, expected_offset, full_scale=2047,
                      min_relative_db=-40):
    """Require a spectral peak near the peer, above noise and unrelated spurs.

    The relative threshold limits how far below the strongest received tone the
    peer may be. Set it from the expected RF path; a median-only noise estimate
    can be arbitrarily small for periodic quantized captures.
    """
    if (not math.isfinite(rate) or rate <= 0 or full_scale <= 0
            or not math.isfinite(expected_offset) or abs(expected_offset) >= rate / 2
            or not math.isfinite(min_relative_db) or min_relative_db > 0):
        raise ValueError("invalid rate, offset, full scale, or relative tone threshold")
    raw = np.fromfile(path, dtype="<i2")
    if len(raw) < 2 * 16384 or len(raw) % 2:
        raise AssertionError(f"capture has only {len(raw) // 2} complex samples")
    iq = raw.reshape(-1, 2)
    window = 16384
    selected = (len(iq) // window) // 2
    block = iq[selected * window:(selected + 1) * window]
    samples = block[:, 0].astype(np.float64) + 1j * block[:, 1]
    samples -= np.mean(samples)
    spectrum = np.abs(np.fft.fft(samples * np.hanning(window)))
    frequencies = np.fft.fftfreq(window, 1 / rate)
    tolerance_hz = max(20000, 2 * rate / window)
    # Search actual local maxima throughout the allowed frequency range. Merely
    # choosing a bin next to the target makes the frequency assertion tautological.
    peaks = ((spectrum > np.roll(spectrum, 1))
             & (spectrum >= np.roll(spectrum, -1)))
    candidates = np.flatnonzero(
        peaks & (np.abs(frequencies - expected_offset) <= tolerance_hz))
    if not candidates.size:
        raise AssertionError("no peer tone peak within frequency tolerance")
    peak_bin = int(candidates[np.argmax(spectrum[candidates])])
    peak_hz = float(frequencies[peak_bin])
    # Compare the Hann main lobe with noise over the same bandwidth. A single
    # unusually high noise bin can pass a median-only amplitude test when the
    # search covers hundreds of bins.
    tone_bins = [(peak_bin + offset) % window for offset in range(-2, 3)]
    tone_power = float(np.sum(spectrum[tone_bins] ** 2))
    noise_power = len(tone_bins) * float(np.median(spectrum ** 2))
    tone_db = 10 * math.log10(tone_power / max(noise_power, 1e-24))
    relative_db = 20 * math.log10(float(spectrum[peak_bin]) /
                                  max(float(np.max(spectrum)), 1e-12))
    peak_fraction = float(np.max(np.abs(block.astype(np.int32)))) / full_scale
    clipped_fraction = float(np.mean(np.abs(block.astype(np.int32)) >= full_scale))
    print(f"MEASURE peer peak={peak_hz:+.1f} Hz, expected={expected_offset:+.1f} Hz, "
          f"tone/noise={tone_db:.1f} dB, peer/strongest={relative_db:.1f} dB, "
          f"peak/FS={peak_fraction:.5f}, "
          f"clipped={clipped_fraction:.6f}, window={selected}/{len(iq) // window}")
    if abs(peak_hz - expected_offset) > tolerance_hz:
        raise AssertionError("peer tone frequency outside tolerance")
    if tone_db < 10:
        raise AssertionError("peer tone is less than 10 dB above median noise")
    if relative_db < min_relative_db:
        raise AssertionError(
            f"peer tone is below {min_relative_db:g} dB relative to the strongest tone")
    if peak_fraction >= 0.8 or clipped_fraction > 0:
        raise AssertionError("RX level is too high or samples are clipped")


def validate(args):
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    dut_capture = output_dir / f"duplex_{args.dut_serial}.cs16"
    peer_capture = output_dir / f"duplex_{args.peer_serial}.cs16"
    dut_log = output_dir / f"duplex_{args.dut_serial}.log"
    peer_log = output_dir / f"duplex_{args.peer_serial}.log"

    def command(serial, output, duration, offset, tx_gain, rx_gain,
                rx_channel, tx_channel):
        return [
            sys.executable, "-u", str(REPO / "examples/python/rx_tx_samples.py"),
            str(output), "--serial", serial, "--rx-channel", str(rx_channel),
            "--tx-channel", str(tx_channel), "--rate", str(args.rate),
            "--frequency", str(args.frequency), "--offset", str(offset),
            "--rx-gain", str(rx_gain), "--attenuation", str(tx_gain),
            "--duration", str(duration), "--timeout-us", "200000",
        ]

    dut_cmd = command(args.dut_serial, dut_capture, args.dut_duration,
                      args.dut_offset, args.dut_tx_gain, args.dut_rx_gain,
                      args.dut_rx_channel, args.dut_tx_channel)
    peer_cmd = command(args.peer_serial, peer_capture, args.peer_duration,
                       args.peer_offset, args.peer_tx_gain, args.peer_rx_gain,
                       args.peer_rx_channel, args.peer_tx_channel)
    print(f"DUT command: {' '.join(dut_cmd)}", flush=True)
    print(f"Peer command: {' '.join(peer_cmd)}", flush=True)
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONUNBUFFERED"] = "1"
    with dut_log.open("w") as dut_output, peer_log.open("w") as peer_output:
        dut = subprocess.Popen(dut_cmd, cwd=REPO, env=env, stdout=dut_output,
                               stderr=subprocess.STDOUT)
        peer = None
        try:
            wait_for_marker(dut, dut_log, "Transmitting a", args.startup_timeout)
            peer = subprocess.Popen(peer_cmd, cwd=REPO, env=env, stdout=peer_output,
                                    stderr=subprocess.STDOUT)
            peer_status = peer.wait(timeout=args.startup_timeout + args.peer_duration)
            dut_status = dut.wait(timeout=args.startup_timeout + args.dut_duration)
        finally:
            for process in (peer, dut):
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f"DUT exit={dut_status}, peer exit={peer_status}; logs: {dut_log}, {peer_log}")
    if dut_status or peer_status:
        raise RuntimeError("full-duplex process failed; inspect child logs")
    print("DUT RX measurement:")
    measure_peer_tone(dut_capture, args.rate, args.peer_offset,
                      args.dut_rx_full_scale, args.min_peer_relative_db)
    print("Peer RX measurement:")
    measure_peer_tone(peer_capture, args.rate, args.dut_offset,
                      args.peer_rx_full_scale, args.min_peer_relative_db)
    print("PASS both cards transmitted and received during full duplex")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dut-serial", default="8N55")
    parser.add_argument("--peer-serial", default="47479")
    parser.add_argument("--dut-rx-channel", type=int, default=0)
    parser.add_argument("--dut-tx-channel", type=int, default=0)
    parser.add_argument("--peer-rx-channel", type=int, default=0)
    parser.add_argument("--peer-tx-channel", type=int, default=0)
    parser.add_argument("--dut-rx-full-scale", type=int, default=2047)
    parser.add_argument("--peer-rx-full-scale", type=int, default=2047)
    parser.add_argument("--min-peer-relative-db", type=float, default=-40,
                        help="Minimum peer peak relative to the strongest tone (dB)")
    parser.add_argument("--startup-timeout", type=float, default=6)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--dut-offset", type=float, default=250e3)
    parser.add_argument("--peer-offset", type=float, default=-300e3)
    parser.add_argument("--dut-tx-gain", type=float, default=80)
    parser.add_argument("--peer-tx-gain", type=float, default=89.75)
    parser.add_argument("--dut-rx-gain", type=float, default=0)
    parser.add_argument("--peer-rx-gain", type=float, default=0)
    parser.add_argument("--dut-duration", type=float, default=6)
    parser.add_argument("--peer-duration", type=float, default=4)
    parser.add_argument("--output-dir", default="build/integration-evidence")
    args = parser.parse_args()
    if not math.isfinite(args.min_peer_relative_db) or args.min_peer_relative_db > 0:
        parser.error("--min-peer-relative-db must be finite and nonpositive")
    return args


if __name__ == "__main__":
    validate(parse_args())
