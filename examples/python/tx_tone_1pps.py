#!/usr/bin/env python3

"""Transmit a tone starting on an external one-pulse-per-second edge.

This example uses SoapySidekiq's timed-activation behavior. An external PPS
signal must be connected and active before running it.
"""

import argparse
import sys
import time

import SoapySDR
from SoapySDR import (
    SOAPY_SDR_CF32,
    SOAPY_SDR_CS16,
    SOAPY_SDR_HAS_TIME,
    SOAPY_SDR_TX,
)

from tx_tone import device_arguments, make_tone, write_all


def transmit_on_pps(args):
    """Configure TX and start the stream on the next external PPS edge."""
    sdr = SoapySDR.Device(device_arguments(args))
    stream = None
    active = False

    try:
        sdr.setTimeSource("1pps_source_external")
        sdr.setSampleRate(SOAPY_SDR_TX, args.channel, args.rate)
        sdr.setBandwidth(
            SOAPY_SDR_TX,
            args.channel,
            args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
        )
        sdr.setFrequency(SOAPY_SDR_TX, args.channel, args.frequency)
        sdr.setGain(SOAPY_SDR_TX, args.channel, args.attenuation)

        stream_format = (
            SOAPY_SDR_CF32 if args.sample_format == "CF32" else SOAPY_SDR_CS16
        )
        stream = sdr.setupStream(SOAPY_SDR_TX, stream_format, [args.channel])
        mtu = sdr.getStreamMTU(stream)
        offset = args.offset if args.offset is not None else args.rate / 4.0
        full_scale = int(sdr.readSetting("full_scale"))
        samples = make_tone(mtu, args.sample_format, offset, args.rate, full_scale)
        scalars_per_element = 1 if args.sample_format == "CF32" else 2

        # SoapySidekiq interprets timed activation as start-on-next-PPS.
        result = sdr.activateStream(stream, SOAPY_SDR_HAS_TIME)
        if result < 0:
            raise RuntimeError(
                f"activateStream failed: {SoapySDR.errToStr(result)} ({result})"
            )
        active = True

        print(
            f"Transmitting a {offset:,.0f} Hz tone after the PPS start; "
            "press Ctrl-C to stop"
        )
        started = time.monotonic()
        while args.duration == 0 or time.monotonic() - started < args.duration:
            write_all(sdr, stream, samples, mtu, scalars_per_element)
    except KeyboardInterrupt:
        print("\nStopping transmission")
    finally:
        if stream is not None:
            if active:
                sdr.deactivateStream(stream)
            sdr.closeStream(stream)


def parse_arguments(argv):
    """Parse command-line options for the PPS tone example."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--tx-block-size", type=int, default=16380)
    parser.add_argument("-r", "--rate", type=float, default=40e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("--offset", type=float)
    parser.add_argument("-a", "--attenuation", type=float, default=35)
    parser.add_argument(
        "--format",
        dest="sample_format",
        choices=("CS16", "CF32"),
        default="CS16",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0,
        help="Seconds to transmit; zero runs until Ctrl-C",
    )
    args = parser.parse_args(argv)
    if args.duration < 0:
        parser.error("--duration cannot be negative")
    if args.tx_block_size <= 0:
        parser.error("--tx-block-size must be positive")
    return args


if __name__ == "__main__":
    transmit_on_pps(parse_arguments(sys.argv[1:]))
