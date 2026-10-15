#!/usr/bin/env python3

"""Transmit interleaved little-endian CS16 samples from a binary file.

The final block is zero-padded to one stream MTU when necessary. By default
the file is transmitted once; pass --repeat to continue until Ctrl-C.
"""

import argparse
import sys

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_TX


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


def write_block(sdr, stream, samples, count):
    """Write a CS16 block while accepting valid partial writes."""
    written = 0
    while written < count:
        result = sdr.writeStream(
            stream,
            [samples[written * 2 :]],
            count - written,
        )
        if result.ret < 0:
            raise RuntimeError(
                f"writeStream failed: {SoapySDR.errToStr(result.ret)} ({result.ret})"
            )
        if result.ret == 0:
            raise RuntimeError("writeStream returned zero samples")
        written += result.ret


def transmit_file(args):
    """Configure TX and transmit the selected sample file."""
    sdr = SoapySDR.Device(device_arguments(args))
    stream = None
    active = False

    try:
        sdr.setSampleRate(SOAPY_SDR_TX, args.channel, args.rate)
        sdr.setBandwidth(
            SOAPY_SDR_TX,
            args.channel,
            args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
        )
        sdr.setFrequency(SOAPY_SDR_TX, args.channel, args.frequency)
        sdr.setGain(SOAPY_SDR_TX, args.channel, args.attenuation)

        stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, [args.channel])
        mtu = sdr.getStreamMTU(stream)
        values_per_block = mtu * 2

        result = sdr.activateStream(stream)
        if result < 0:
            raise RuntimeError(
                f"activateStream failed: {SoapySDR.errToStr(result)} ({result})"
            )
        active = True

        transmitted = 0
        while True:
            saw_samples = False
            with open(args.input, "rb") as sample_file:
                while True:
                    values = np.fromfile(
                        sample_file, dtype="<i2", count=values_per_block
                    )
                    if values.size == 0:
                        break
                    if values.size % 2 != 0:
                        raise ValueError("CS16 input contains an incomplete I/Q sample")

                    saw_samples = True
                    valid_samples = values.size // 2
                    if values.size < values_per_block:
                        padded = np.zeros(values_per_block, dtype=np.int16)
                        padded[: values.size] = values
                        values = padded
                    write_block(sdr, stream, values, mtu)
                    transmitted += valid_samples

            if not saw_samples:
                raise ValueError(f"input file is empty: {args.input}")
            if not args.repeat:
                break

        print(f"Transmitted {transmitted:,} input samples from {args.input}")
    except KeyboardInterrupt:
        print("\nStopping transmission")
    finally:
        if stream is not None:
            if active:
                sdr.deactivateStream(stream)
            sdr.closeStream(stream)


def parse_arguments(argv):
    """Parse command-line options for the file-transmit example."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Binary file containing interleaved CS16 samples")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("-r", "--rate", type=float, default=40e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("-a", "--attenuation", type=float, default=0)
    parser.add_argument(
        "--repeat", action="store_true", help="Repeat the file until Ctrl-C"
    )
    return parser.parse_args(argv)


if __name__ == "__main__":
    transmit_file(parse_arguments(sys.argv[1:]))
