#!/usr/bin/env python3

"""Check CF32 conversion of real Sidekiq FPGA counter samples."""

import argparse
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_RX

from cs16_validate import check_counter, device_arguments


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

    stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [args.channel])
    active = False
    try:
        mtu = sdr.getStreamMTU(stream)
        buffer = np.empty(mtu, dtype=np.complex64)
        result = sdr.activateStream(stream)
        if result < 0:
            raise RuntimeError(f"activateStream failed: {result}")
        active = True

        expected_counter = None
        total = 0
        for block in range(args.blocks):
            result = sdr.readStream(stream, [buffer], mtu, timeoutUs=args.timeout_us)
            if result.ret <= 0:
                raise RuntimeError(f"CF32 read {block + 1} failed: {result.ret}")
            # The driver scales each signed counter scalar by full_scale.
            scalars = buffer[: result.ret].view(np.float32)
            recovered = np.rint(scalars * full_scale).astype(np.int32)
            if not np.allclose(scalars * full_scale, recovered, atol=0.01):
                raise AssertionError(f"CF32 read {block + 1} lost integer precision")
            expected_counter = check_counter(recovered, full_scale, expected_counter)
            total += result.ret

        print(f"PASS CF32: {args.blocks} reads, {total:,} complex samples")
    finally:
        try:
            if active:
                sdr.deactivateStream(stream)
        finally:
            sdr.closeStream(stream)


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
