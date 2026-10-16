#!/usr/bin/env python3

"""Retune PCIe RX while a selected peer transmits from a separate process."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT

from paired_tone_validate import REPO, measure_capture, wait_for_marker


def validate(args):
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    tx_log = output_dir / f"retune_{args.tx_serial}_tx.log"
    tx_cmd = [sys.executable, "-u", str(REPO / "examples/python/tx_tone.py"),
              "--serial", args.tx_serial, "--channel", str(args.tx_channel),
              "--format", "CS16",
              "--rate", str(args.rate), "--frequency", str(args.frequency),
              "--offset", str(args.tone_offset), "--attenuation", str(args.tx_gain),
              "--duration", str(args.tx_duration)]
    print(f"TX command: {' '.join(tx_cmd)}", flush=True)
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    with tx_log.open("w") as tx_output:
        tx = subprocess.Popen(tx_cmd, cwd=REPO, env=env, stdout=tx_output,
                              stderr=subprocess.STDOUT)
        try:
            wait_for_marker(tx, tx_log, "Transmitting a", args.startup_timeout)
            sdr = SoapySDR.Device({"driver": "sidekiq", "serial": "47479"})
            stream = None
            active = False
            try:
                sdr.setSampleRate(SOAPY_SDR_RX, 0, args.rate)
                sdr.setBandwidth(SOAPY_SDR_RX, 0, args.rate * 0.8)
                sdr.setGainMode(SOAPY_SDR_RX, 0, False)
                sdr.setGain(SOAPY_SDR_RX, 0, args.rx_gain)
                stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
                mtu = sdr.getStreamMTU(stream)
                buffer = np.empty(mtu * 2, dtype="<i2")
                for index, offset in enumerate((0, 100000), start=1):
                    if tx.poll() is not None:
                        raise AssertionError("TX exited before RX retuning completed")
                    lo = args.frequency + offset
                    sdr.setFrequency(SOAPY_SDR_RX, 0, lo)
                    result = sdr.activateStream(stream)
                    if result < 0:
                        raise RuntimeError(f"RX activateStream failed: {result}")
                    active = True
                    capture = output_dir / f"retune_{index}.cs16"
                    received = 0
                    deadline = time.monotonic() + 0.8
                    with capture.open("wb") as output:
                        while time.monotonic() < deadline:
                            result = sdr.readStream(stream, [buffer], mtu, timeoutUs=200000)
                            if result.ret == SOAPY_SDR_TIMEOUT or result.ret == 0:
                                continue
                            if result.ret < 0:
                                raise RuntimeError(f"RX readStream failed: {result.ret}")
                            output.write(buffer[:result.ret * 2].tobytes())
                            received += result.ret
                    sdr.deactivateStream(stream)
                    active = False
                    print(f"RX LO={lo:.0f} Hz, samples={received}, TX alive={tx.poll() is None}")
                    measure_capture(capture, args.rate, args.tone_offset - offset, 2047)
                if tx.poll() is not None:
                    raise AssertionError("TX exited during RX retuning")
            finally:
                if active:
                    sdr.deactivateStream(stream)
                if stream is not None:
                    sdr.closeStream(stream)
            tx_status = tx.wait(timeout=args.tx_duration + 8)
            if tx_status:
                raise RuntimeError(f"TX process exited {tx_status}; inspect {tx_log}")
        finally:
            if tx.poll() is None:
                tx.terminate()
                try:
                    tx.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    tx.kill()
                    tx.wait()
    print(f"PASS PCIe RX retuned while {args.tx_serial} TX process remained active")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-serial", default="8N55")
    parser.add_argument("--tx-channel", type=int, default=0)
    parser.add_argument("--startup-timeout", type=float, default=6)
    parser.add_argument("--tx-duration", type=float, default=6)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--tone-offset", type=float, default=250e3)
    parser.add_argument("--tx-gain", type=float, default=80)
    parser.add_argument("--rx-gain", type=float, default=20)
    parser.add_argument("--output-dir", default="build/integration-evidence/retune")
    return parser.parse_args()


if __name__ == "__main__":
    validate(parse_args())
