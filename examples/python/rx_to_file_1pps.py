#!/usr/bin/env python3

"""Capture finite CS16 RX samples starting on the next external PPS edge.

Connect an active external 1 PPS source before running this example. The output
contains little-endian interleaved signed 16-bit I/Q values.
The PPS edge starts streaming but does not reset the hardware timestamp.
"""

import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME, SOAPY_SDR_RX

from rx_to_file import device_arguments, require_success


def capture_on_pps(args):
    sdr = SoapySDR.Device(device_arguments(args))
    stream = None
    active = False

    try:
        sdr.setTimeSource("1pps_source_external")
        sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
        sdr.setBandwidth(
            SOAPY_SDR_RX,
            args.channel,
            args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
        )
        sdr.setFrequency(SOAPY_SDR_RX, args.channel, args.frequency)
        if args.gain is None:
            sdr.setGainMode(SOAPY_SDR_RX, args.channel, True)
        else:
            sdr.setGainMode(SOAPY_SDR_RX, args.channel, False)
            sdr.setGain(SOAPY_SDR_RX, args.channel, args.gain)

        stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [args.channel])
        mtu = sdr.getStreamMTU(stream)
        buffer = np.empty(mtu * 2, dtype="<i2")
        total_requested = mtu * args.blocks

        print("Waiting for the next external PPS edge", flush=True)
        started = time.monotonic()
        require_success("activateStream", sdr.activateStream(stream, SOAPY_SDR_HAS_TIME))
        active = True
        print(f"RX started after {time.monotonic() - started:.3f} seconds")

        received = 0
        first_time_ns = None
        with open(args.output, "wb") as output:
            while received < total_requested:
                result = sdr.readStream(
                    stream, [buffer], min(mtu, total_requested - received),
                    timeoutUs=args.timeout_us,
                )
                require_success("readStream", result.ret)
                if result.ret == 0:
                    raise RuntimeError("readStream returned zero samples")
                if first_time_ns is None:
                    if not result.flags & SOAPY_SDR_HAS_TIME:
                        raise RuntimeError("First RX block has no hardware timestamp")
                    first_time_ns = result.timeNs
                output.write(buffer[: result.ret * 2].tobytes())
                received += result.ret

        print(
            f"Captured {received:,} complex samples ({received * 4:,} bytes) "
            f"to {args.output}"
        )
        print(f"First sample timestamp: {first_time_ns:,} ns")
    finally:
        if stream is not None:
            try:
                if active:
                    sdr.deactivateStream(stream)
            finally:
                sdr.closeStream(stream)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="pps_samples.bin")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--bandwidth", type=float)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--gain", type=float)
    parser.add_argument("--blocks", type=int, default=100)
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    if args.rate <= 0 or args.blocks <= 0 or args.timeout_us <= 0:
        parser.error("--rate, --blocks, and --timeout-us must be positive")
    return args


if __name__ == "__main__":
    capture_on_pps(parse_arguments(sys.argv[1:]))
