#!/usr/bin/env python3

"""Transmit a finite tone on one card and measure it on another card."""

import argparse
import math
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np


REPO = Path(__file__).resolve().parents[2]


def wait_for_marker(process, log_path, marker, seconds=6):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"process exited before {marker}: {process.returncode}")
        if marker in log_path.read_text(errors="replace"):
            return time.monotonic()
        time.sleep(0.05)
    raise TimeoutError(f"process did not report {marker} within {seconds} seconds")


def measure_capture(path, rate, expected_offset, full_scale, fraction=0.5):
    raw = np.fromfile(path, dtype="<i2")
    if len(raw) < 2 * 16384 or len(raw) % 2:
        raise AssertionError(f"capture has only {len(raw) // 2} complex samples")
    iq = raw.reshape(-1, 2)
    window = 16384
    windows = len(iq) // window
    if not 0 <= fraction < 1:
        raise ValueError("capture fraction must be between 0 and 1")
    selected = min(windows - 1, int(windows * fraction))
    block = iq[selected * window : (selected + 1) * window]
    complex_samples = block[:, 0].astype(np.float64) + 1j * block[:, 1]
    complex_samples -= np.mean(complex_samples)
    rms_fraction = float(np.sqrt(np.mean(np.abs(complex_samples) ** 2))) / full_scale
    spectrum = np.abs(np.fft.fft(complex_samples * np.hanning(window)))
    peak_bin = int(np.argmax(spectrum))
    peak_hz = float(np.fft.fftfreq(window, 1 / rate)[peak_bin])
    noise = float(np.median(spectrum))
    tone_db = 20 * math.log10(float(spectrum[peak_bin]) / max(noise, 1e-12))
    peak_fraction = float(np.max(np.abs(block.astype(np.int32)))) / full_scale
    clipped_fraction = float(np.mean(np.abs(block.astype(np.int32)) >= full_scale))
    crest = peak_fraction / max(rms_fraction, 1e-12)
    tolerance_hz = max(20000, 2 * rate / window)
    print(
        f"MEASURE peak={peak_hz:+.1f} Hz, expected={expected_offset:+.1f} Hz, "
        f"tone/noise={tone_db:.1f} dB, RMS/FS={rms_fraction:.5f}, "
        f"peak/FS={peak_fraction:.5f}, crest={crest:.2f}, "
        f"clipped={clipped_fraction:.6f}, "
        f"window={selected}/{windows}"
    )
    if abs(peak_hz - expected_offset) > tolerance_hz:
        raise AssertionError(f"tone frequency outside ±{tolerance_hz:.0f} Hz")
    if tone_db < 10:
        raise AssertionError("tone is less than 10 dB above median spectral noise")
    if peak_fraction >= 0.8 or clipped_fraction > 0:
        raise AssertionError("RX level is too high or samples are clipped")
    if rms_fraction >= 0.005 and crest > 4:
        raise AssertionError("continuous tone has intermittent high peaks")


def validate(args):
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    rx_log = output.with_suffix(output.suffix + ".rx.log")
    tx_log = output.with_suffix(output.suffix + ".tx.log")
    rx_cmd = [
        sys.executable, "-u", str(REPO / "examples/python/record_rx_to_file.py"),
        str(output), "--serial", args.rx_serial, "--channel", str(args.rx_channel),
        "--rate", str(args.rate), "--frequency", str(args.frequency),
        "--gain", str(args.rx_gain), "--duration", str(args.rx_duration),
        "--timeout-us", "200000",
    ]
    tx_cmd = [
        sys.executable, "-u", str(REPO / args.tx_script),
        "--serial", args.tx_serial, "--channel", str(args.tx_channel),
        "--rate", str(args.rate), "--frequency", str(args.frequency),
        "--offset", str(args.offset), "--attenuation", str(args.tx_gain),
        "--format", args.tx_format, "--duration", str(args.tx_duration),
    ]
    print(f"RX command: {' '.join(rx_cmd)}", flush=True)
    print(f"TX command: {' '.join(tx_cmd)}", flush=True)
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    with rx_log.open("w") as rx_output, tx_log.open("w") as tx_output:
        receiver = None
        transmitter = None
        try:
            capture_fraction = 0.5
            if args.start_order == "rx-first":
                receiver = subprocess.Popen(rx_cmd, cwd=REPO, env=env, stdout=rx_output,
                                            stderr=subprocess.STDOUT)
                rx_ready = wait_for_marker(receiver, rx_log, "Recording to",
                                           args.startup_timeout)
                transmitter = subprocess.Popen(tx_cmd, cwd=REPO, env=env,
                                               stdout=tx_output, stderr=subprocess.STDOUT)
                tx_ready = wait_for_marker(transmitter, tx_log, "Transmitting a",
                                           args.startup_timeout)
                elapsed = tx_ready - rx_ready
                overlap = min(args.tx_duration, args.rx_duration - elapsed)
                if overlap <= 0.4:
                    raise AssertionError(
                        f"RX/TX overlap too short: {overlap:.3f} seconds"
                    )
                capture_fraction = (elapsed + overlap / 2) / args.rx_duration
                print(f"RX/TX marker delay={elapsed:.3f}s, overlap={overlap:.3f}s, "
                      f"capture fraction={capture_fraction:.3f}", flush=True)
            else:
                transmitter = subprocess.Popen(tx_cmd, cwd=REPO, env=env,
                                               stdout=tx_output, stderr=subprocess.STDOUT)
                wait_for_marker(transmitter, tx_log, "Transmitting a",
                                args.startup_timeout)
                receiver = subprocess.Popen(rx_cmd, cwd=REPO, env=env, stdout=rx_output,
                                            stderr=subprocess.STDOUT)
            tx_status = transmitter.wait(timeout=args.tx_duration + 8)
            rx_status = receiver.wait(timeout=args.rx_duration + 8)
        finally:
            for process in (transmitter, receiver):
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f"RX exit={rx_status}, TX exit={tx_status}; logs: {rx_log}, {tx_log}", flush=True)
    if rx_status or tx_status:
        raise RuntimeError("paired RX or TX process failed; inspect child logs")
    measure_capture(output, args.rate, args.offset, args.rx_full_scale,
                    capture_fraction)
    print("PASS paired tone frequency and level", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", required=True)
    parser.add_argument("--tx-script", default="examples/python/tx_tone.py")
    parser.add_argument("--rx-serial", required=True)
    parser.add_argument("--tx-channel", type=int, default=0)
    parser.add_argument("--rx-channel", type=int, default=0)
    parser.add_argument("--tx-format", choices=("CS16", "CF32"), default="CS16")
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--offset", type=float, default=250e3)
    parser.add_argument("--tx-gain", type=float, default=0)
    parser.add_argument("--rx-gain", type=float, default=0)
    parser.add_argument("--start-order", choices=("rx-first", "tx-first"),
                        default="rx-first")
    parser.add_argument("--startup-timeout", type=float, default=6)
    parser.add_argument("--tx-duration", type=float, default=1)
    parser.add_argument("--rx-duration", type=float, default=3)
    parser.add_argument("--rx-full-scale", type=int, default=2047)
    parser.add_argument("--output", default="build/integration-evidence/paired_tone.cs16")
    args = parser.parse_args()
    if (args.rate <= 0 or args.rx_full_scale <= 0 or args.tx_duration <= 0
            or args.rx_duration <= 0 or args.startup_timeout <= 0):
        parser.error("rate, RX full scale, and durations must be positive")
    return args


if __name__ == "__main__":
    validate(parse_args())
