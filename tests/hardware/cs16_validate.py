#!/usr/bin/env python3

"""Check real CS16 counter samples and RX timestamps on a Sidekiq card."""

import argparse
import math
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_HAS_TIME, SOAPY_SDR_RX


def device_arguments(args):
    result = {"driver": "sidekiq"}
    result["serial" if args.serial else "card"] = args.serial or args.card
    if args.topology:
        result["topology"] = args.topology
    return result


def check_counter(samples, full_scale, expected_start):
    values = samples.astype(np.int32)
    span = 2 * (full_scale + 1)
    start = int(values[0]) if expected_start is None else expected_start
    expected = (start + np.arange(len(values)) + full_scale + 1) % span
    expected -= full_scale + 1
    mismatch = np.flatnonzero(values != expected)
    if mismatch.size:
        index = int(mismatch[0])
        raise AssertionError(
            f"counter mismatch at scalar {index}: expected {expected[index]}, "
            f"received {values[index]}"
        )
    return int((int(values[-1]) + 1 + full_scale + 1) % span - full_scale - 1)


def validate_source(sdr, args, source, full_scale):
    sdr.writeSetting("timetype", source)
    stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [args.channel])
    active = False
    try:
        mtu = sdr.getStreamMTU(stream)
        buffer = np.empty(mtu * 2, dtype="<i2")
        sdr.setHardwareTime(0, "now")
        result = sdr.activateStream(stream)
        if result < 0:
            raise RuntimeError(f"activateStream failed: {result}")
        active = True

        expected_counter = None
        previous_time = None
        previous_count = 0
        total = 0
        sys_freq = int(sdr.readSetting("sys_clock_freq"))
        tick_ns = 1e9 / (args.rate if source == "rf_timestamp" else sys_freq)
        for block in range(args.blocks):
            result = sdr.readStream(stream, [buffer], mtu, timeoutUs=args.timeout_us)
            if result.ret <= 0:
                raise RuntimeError(f"{source} read {block + 1} failed: {result.ret}")
            if not result.flags & SOAPY_SDR_HAS_TIME:
                raise AssertionError(f"{source} read {block + 1} has no timestamp")

            expected_counter = check_counter(
                buffer[: result.ret * 2], full_scale, expected_counter
            )
            if previous_time is not None:
                expected_time = previous_time + previous_count * 1e9 / args.rate
                tolerance_ns = math.ceil(tick_ns) + 1
                if abs(result.timeNs - expected_time) > tolerance_ns:
                    raise AssertionError(
                        f"{source} timestamp on read {block + 1}: "
                        f"expected {expected_time:.0f} ±{tolerance_ns} ns, "
                        f"received {result.timeNs} ns"
                    )
            previous_time = result.timeNs
            previous_count = result.ret
            total += result.ret

        print(f"PASS {source}: {args.blocks} reads, {total:,} CS16 samples")
    finally:
        try:
            if active:
                sdr.deactivateStream(stream)
        finally:
            sdr.closeStream(stream)


def validate(args):
    sdr = SoapySDR.Device(device_arguments(args))
    sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
    sdr.setBandwidth(
        SOAPY_SDR_RX, args.channel,
        args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
    )
    sdr.setFrequency(SOAPY_SDR_RX, args.channel, args.frequency)
    sdr.writeSetting("iq_swap", "false")
    sdr.writeSetting("counter", "true")
    if sdr.readSetting("counter") != "true":
        raise AssertionError("counter source was not enabled")
    full_scale = int(sdr.readSetting("full_scale"))

    for source in ("rf_timestamp", "sys_timestamp"):
        validate_source(sdr, args, source, full_scale)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--card", default="0")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--bandwidth", type=float)
    parser.add_argument("--frequency", type=float, default=1e9)
    parser.add_argument("--blocks", type=int, default=10)
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    if args.rate <= 0 or args.blocks <= 0 or args.timeout_us <= 0:
        parser.error("--rate, --blocks, and --timeout-us must be positive")
    return args


if __name__ == "__main__":
    validate(parse_arguments(sys.argv[1:]))
