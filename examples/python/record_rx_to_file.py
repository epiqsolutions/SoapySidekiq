#!/usr/bin/env python3

"""Continuously record received CS16 samples to a binary file.

The output contains little-endian, interleaved signed 16-bit I/Q values.
Press Ctrl-C to stop, or use --duration for a finite recording.
"""

import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT


def device_arguments(args):
    """Build the keyword arguments used to open a Sidekiq device."""
    result = {"driver": "sidekiq"}
    if args.serial is None:
        result["card"] = args.card
    else:
        result["serial"] = args.serial
    if args.topology:
        result["topology"] = args.topology
    return result


def record(args):
    """Configure RX and record samples until interrupted or timed out."""
    sdr = SoapySDR.Device(device_arguments(args))
    stream = None
    active = False

    try:
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

        result = sdr.activateStream(stream)
        if result < 0:
            raise RuntimeError(
                f"activateStream failed: {SoapySDR.errToStr(result)} ({result})"
            )
        active = True

        received = 0
        started = time.monotonic()
        print(f"Recording to {args.output}; press Ctrl-C to stop")
        with open(args.output, "wb") as output:
            while args.duration == 0 or time.monotonic() - started < args.duration:
                result = sdr.readStream(
                    stream,
                    [buffer],
                    mtu,
                    timeoutUs=args.timeout_us,
                )
                if result.ret == SOAPY_SDR_TIMEOUT:
                    continue
                if result.ret < 0:
                    raise RuntimeError(
                        f"readStream failed: {SoapySDR.errToStr(result.ret)} "
                        f"({result.ret})"
                    )
                if result.ret == 0:
                    continue

                output.write(buffer[: result.ret * 2].tobytes())
                received += result.ret

        print(f"Recorded {received:,} complex samples ({received * 4:,} bytes)")
    except KeyboardInterrupt:
        print("\nStopping recording")
    finally:
        if stream is not None:
            if active:
                sdr.deactivateStream(stream)
            sdr.closeStream(stream)


def parse_arguments(argv):
    """Parse command-line options for the continuous recording example."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="samples.bin")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("-r", "--rate", type=float, default=2e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("-g", "--gain", type=float)
    parser.add_argument(
        "--duration",
        type=float,
        default=0,
        help="Seconds to record; zero runs until Ctrl-C",
    )
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    if args.duration < 0:
        parser.error("--duration cannot be negative")
    if args.timeout_us < 0:
        parser.error("--timeout-us cannot be negative")
    return args


if __name__ == "__main__":
    record(parse_arguments(sys.argv[1:]))
