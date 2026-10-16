#!/usr/bin/env python3

"""Bounded external-PPS RX/TX starts, stops, and pending-TX cleanup."""

import argparse
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME, SOAPY_SDR_RX, SOAPY_SDR_TX


def require_ok(label, result):
    if result < 0:
        raise RuntimeError(f"{label} failed: {result}")


def validate(serial, rate, rx_channel, tx_channel):
    sdr = SoapySDR.Device({"driver": "sidekiq", "serial": serial})
    sdr.setTimeSource("1pps_source_external")
    if sdr.getTimeSource() != "1pps_source_external":
        raise AssertionError("external PPS source readback differs")
    print(f"PPS source confirmed for {serial}", flush=True)

    for direction, channel in ((SOAPY_SDR_RX, rx_channel),
                               (SOAPY_SDR_TX, tx_channel)):
        sdr.setSampleRate(direction, channel, rate)
        sdr.setBandwidth(direction, channel, rate * 0.8)
        sdr.setFrequency(direction, channel, 1e9)
    sdr.setGainMode(SOAPY_SDR_RX, rx_channel, False)
    sdr.setGain(SOAPY_SDR_RX, rx_channel, 0)
    sdr.setGain(SOAPY_SDR_TX, tx_channel, 0)

    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [rx_channel])
    rx_active = False
    try:
        rx_mtu = sdr.getStreamMTU(rx)
        rx_buffer = np.empty(rx_mtu * 2, dtype="<i2")
        t0 = time.monotonic()
        require_ok("timed RX start", sdr.activateStream(rx, SOAPY_SDR_HAS_TIME))
        rx_active = True
        start_wait = time.monotonic() - t0
        block = sdr.readStream(rx, [rx_buffer], rx_mtu, timeoutUs=200000)
        require_ok("timed RX read", block.ret)
        if block.ret <= 0 or not block.flags & SOAPY_SDR_HAS_TIME:
            raise AssertionError("timed RX returned no timestamped samples")
        t0 = time.monotonic()
        require_ok("timed RX stop", sdr.deactivateStream(rx, SOAPY_SDR_HAS_TIME))
        rx_active = False
        stop_wait = time.monotonic() - t0
        print(f"RX timed start wait={start_wait:.3f}s, stop wait={stop_wait:.3f}s, "
              f"samples={block.ret}, timestamp={block.timeNs}", flush=True)
        require_ok("RX restart", sdr.activateStream(rx))
        rx_active = True
        block = sdr.readStream(rx, [rx_buffer], rx_mtu, timeoutUs=200000)
        require_ok("RX restart read", block.ret)
        if block.ret <= 0:
            raise AssertionError("RX restart returned no samples")
        require_ok("RX immediate stop", sdr.deactivateStream(rx))
        rx_active = False
        print(f"RX immediate restart/cleanup samples={block.ret}", flush=True)
        require_ok("second timed RX start", sdr.activateStream(rx, SOAPY_SDR_HAS_TIME))
        rx_active = True
        require_ok("timed RX immediate cleanup", sdr.deactivateStream(rx))
        rx_active = False
        print("Timed RX immediate cleanup, host reads=0", flush=True)
        require_ok("RX restart after timed cleanup", sdr.activateStream(rx))
        rx_active = True
        block = sdr.readStream(rx, [rx_buffer], rx_mtu, timeoutUs=200000)
        require_ok("RX read after timed cleanup", block.ret)
        if block.ret <= 0:
            raise AssertionError("RX after timed cleanup returned no samples")
        require_ok("RX stop after timed cleanup", sdr.deactivateStream(rx))
        rx_active = False
        print(f"RX after timed cleanup samples={block.ret}", flush=True)
    finally:
        if rx_active:
            sdr.deactivateStream(rx)
        sdr.closeStream(rx)

    tx = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, [tx_channel])
    tx_active = False
    try:
        tx_mtu = sdr.getStreamMTU(tx)
        tx_buffer = np.zeros(tx_mtu * 2, dtype="<i2")
        require_ok("timed TX start", sdr.activateStream(tx, SOAPY_SDR_HAS_TIME))
        tx_active = True
        t0 = time.monotonic()
        sent = sdr.writeStream(tx, [tx_buffer], tx_mtu, timeoutUs=200000)
        require_ok("timed TX first write", sent.ret)
        if sent.ret != tx_mtu:
            raise AssertionError(f"timed TX wrote {sent.ret} of {tx_mtu}")
        write_wait = time.monotonic() - t0
        t0 = time.monotonic()
        require_ok("timed TX stop", sdr.deactivateStream(tx, SOAPY_SDR_HAS_TIME))
        tx_active = False
        stop_wait = time.monotonic() - t0
        print(f"TX first write wait={write_wait:.3f}s, timed stop wait={stop_wait:.3f}s, "
              f"samples={sent.ret}", flush=True)

        # No host samples are submitted between activation and this stop.
        require_ok("pending TX start", sdr.activateStream(tx, SOAPY_SDR_HAS_TIME))
        tx_active = True
        t0 = time.monotonic()
        require_ok("pending TX immediate cleanup", sdr.deactivateStream(tx))
        tx_active = False
        print(f"Pending TX cleanup wait={time.monotonic() - t0:.3f}s, "
              "submitted samples=0", flush=True)
        require_ok("TX restart", sdr.activateStream(tx))
        tx_active = True
        sent = sdr.writeStream(tx, [tx_buffer], tx_mtu, timeoutUs=200000)
        require_ok("TX restart write", sent.ret)
        if sent.ret != tx_mtu:
            raise AssertionError(f"TX restart wrote {sent.ret} of {tx_mtu}")
        require_ok("TX immediate stop", sdr.deactivateStream(tx))
        tx_active = False
        print(f"TX immediate restart/cleanup samples={sent.ret}", flush=True)
    finally:
        if tx_active:
            sdr.deactivateStream(tx)
        sdr.closeStream(tx)
    print(f"PASS external PPS lifecycle for {serial}", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--rx-channel", type=int, default=0)
    parser.add_argument("--tx-channel", type=int, default=0)
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    validate(args.serial, args.rate, args.rx_channel, args.tx_channel)
