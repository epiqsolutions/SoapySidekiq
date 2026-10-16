#!/usr/bin/env python3

"""Validate partial CS16 reads and timestamp phase against FPGA counter data."""

import argparse
import math

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME, SOAPY_SDR_RX

from cs16_validate import check_counter


def validate(args):
    sdr = SoapySDR.Device({"driver": "sidekiq", "serial": args.serial})
    sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
    sdr.setBandwidth(SOAPY_SDR_RX, args.channel, args.rate * 0.8)
    sdr.setFrequency(SOAPY_SDR_RX, args.channel, args.frequency)
    sdr.writeSetting("iq_swap", "false")
    sdr.writeSetting("counter", "true")
    full_scale = int(sdr.readSetting("full_scale"))

    for source in ("rf_timestamp", "sys_timestamp"):
        sdr.writeSetting("timetype", source)
        stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [args.channel])
        active = False
        try:
            mtu = sdr.getStreamMTU(stream)
            if args.request >= mtu:
                raise ValueError(f"request {args.request} must be below MTU {mtu}")
            buffer = np.empty(args.request * 2, dtype="<i2")
            sys_freq = int(sdr.readSetting("sys_clock_freq"))
            tolerance_ns = math.ceil(1e9 / (args.rate if source == "rf_timestamp" else sys_freq)) + 1
            sdr.setHardwareTime(0, "now")
            status = sdr.activateStream(stream)
            if status < 0:
                raise RuntimeError(f"activateStream failed: {status}")
            active = True

            next_counter = None
            previous_time = None
            previous_count = 0
            total = 0
            for read in range(args.reads):
                result = sdr.readStream(
                    stream, [buffer], args.request, timeoutUs=args.timeout_us
                )
                if result.ret <= 0 or result.ret > args.request:
                    raise AssertionError(f"read {read + 1} returned {result.ret}")
                if not result.flags & SOAPY_SDR_HAS_TIME:
                    raise AssertionError(f"read {read + 1} has no timestamp")
                next_counter = check_counter(
                    buffer[: result.ret * 2], full_scale, next_counter
                )
                if previous_time is not None:
                    expected_time = previous_time + previous_count * 1e9 / args.rate
                    if abs(result.timeNs - expected_time) > tolerance_ns:
                        raise AssertionError(
                            f"{source} read {read + 1}: expected {expected_time:.0f} "
                            f"±{tolerance_ns} ns, received {result.timeNs} ns"
                        )
                previous_time = result.timeNs
                previous_count = result.ret
                total += result.ret
            print(
                f"PASS {source}: {args.reads} partial reads, {total} samples, "
                f"request {args.request} < MTU {mtu}"
            )
        finally:
            try:
                if active:
                    sdr.deactivateStream(stream)
            finally:
                sdr.closeStream(stream)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--request", type=int, default=257)
    parser.add_argument("--reads", type=int, default=20)
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args()
    if args.rate <= 0 or args.request <= 0 or args.reads <= 0 or args.timeout_us <= 0:
        parser.error("rate, request, reads, and timeout must be positive")
    return args


if __name__ == "__main__":
    validate(parse_args())
