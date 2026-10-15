#!/usr/bin/env python3

"""Check repeated activation and deactivation of one hardware RX stream."""

import argparse
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX

from cs16_validate import device_arguments


def validate(args):
    sdr = SoapySDR.Device(device_arguments(args))
    sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
    sdr.setBandwidth(
        SOAPY_SDR_RX, args.channel,
        args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
    )
    sdr.setFrequency(SOAPY_SDR_RX, args.channel, args.frequency)
    sdr.setGainMode(SOAPY_SDR_RX, args.channel, True)

    stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [args.channel])
    active = False
    try:
        mtu = sdr.getStreamMTU(stream)
        buffer = np.empty(mtu * 2, dtype="<i2")
        for cycle in range(1, args.cycles + 1):
            result = sdr.activateStream(stream)
            if result < 0:
                raise RuntimeError(f"cycle {cycle} activateStream failed: {result}")
            active = True
            received = 0
            try:
                for block in range(1, args.blocks_per_cycle + 1):
                    result = sdr.readStream(
                        stream, [buffer], mtu, timeoutUs=args.timeout_us,
                    )
                    if result.ret <= 0:
                        raise RuntimeError(
                            f"cycle {cycle}, read {block} failed: {result.ret}"
                        )
                    received += result.ret
            finally:
                sdr.deactivateStream(stream)
                active = False
            print(f"PASS cycle {cycle}: {received:,} samples")
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
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--blocks-per-cycle", type=int, default=10)
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    if (args.rate <= 0 or args.cycles <= 0 or args.blocks_per_cycle <= 0
            or args.timeout_us <= 0):
        parser.error("--rate, --cycles, --blocks-per-cycle, and --timeout-us must be positive")
    return args


if __name__ == "__main__":
    validate(parse_arguments(sys.argv[1:]))
