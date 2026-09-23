#!/usr/bin/env python3

"""Capture a finite number of CS16 sample blocks to a binary file.

The output contains little-endian, interleaved signed 16-bit I/Q values.
Each read requests no more than the stream MTU and only returned samples are
written, so partial reads never expose uninitialized buffer contents.
"""

import argparse
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX


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


def require_success(operation, result):
    """Raise a readable exception when a SoapySDR status is negative."""
    if result < 0:
        raise RuntimeError(
            f"{operation} failed: {SoapySDR.errToStr(result)} ({result})"
        )


def capture(args):
    """Configure the receiver and capture the requested number of samples."""
    sdr = SoapySDR.Device(device_arguments(args))
    stream = None
    active = False

    try:
        # Configure the radio before setupStream(), because stream setup reads
        # timestamp information derived from the configured sample rate.
        sdr.writeSetting("iq_swap", "true")
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
        total_requested = mtu * args.blocks
        buffer = np.empty(mtu * 2, dtype="<i2")

        require_success("activateStream", sdr.activateStream(stream))
        active = True

        received = 0
        first_time_ns = None
        with open(args.output, "wb") as output:
            while received < total_requested:
                request = min(mtu, total_requested - received)
                result = sdr.readStream(
                    stream,
                    [buffer],
                    request,
                    timeoutUs=args.timeout_us,
                )
                require_success("readStream", result.ret)
                if result.ret == 0:
                    raise RuntimeError("readStream returned zero samples")

                # CS16 has two int16 values per complex stream element.
                output.write(buffer[: result.ret * 2].tobytes())
                received += result.ret
                if first_time_ns is None:
                    first_time_ns = result.timeNs

        print(
            f"Captured {received:,} complex samples ({received * 4:,} bytes) "
            f"to {args.output}"
        )
        if first_time_ns is not None:
            print(f"First sample timestamp: {first_time_ns:,} ns")
    finally:
        if stream is not None:
            if active:
                sdr.deactivateStream(stream)
            sdr.closeStream(stream)


def parse_arguments(argv):
    """Parse command-line options for the finite capture example."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="samples.bin")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("-r", "--rate", type=float, default=40e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("-g", "--gain", type=float)
    parser.add_argument(
        "--blocks", type=int, default=1000, help="Number of MTU-sized blocks"
    )
    parser.add_argument("--timeout-us", type=int, default=100000)
    args = parser.parse_args(argv)
    if args.blocks <= 0:
        parser.error("--blocks must be positive")
    if args.timeout_us < 0:
        parser.error("--timeout-us cannot be negative")
    return args


if __name__ == "__main__":
    capture(parse_arguments(sys.argv[1:]))
