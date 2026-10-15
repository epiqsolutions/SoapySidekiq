#!/usr/bin/env python3

"""Transmit a continuous complex tone with CS16 or CF32 samples.

This is the recommended starting point for a new SoapySidekiq transmit
application. Press Ctrl-C to stop, or use --duration for a finite run.
"""

import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_CS16, SOAPY_SDR_TX


def device_arguments(args):
    """Build the keyword arguments used to open a Sidekiq device."""
    result = {
        "driver": "sidekiq",
        "tx_block_size": str(args.tx_block_size),
    }
    if args.serial is None:
        result["card"] = args.card
    else:
        result["serial"] = args.serial
    if args.topology:
        result["topology"] = args.topology
    return result


def make_tone(count, sample_format, frequency, sample_rate, full_scale):
    """Return one repeatable buffer containing a complex sinusoid."""
    phase = 2.0 * np.pi * frequency * np.arange(count) / sample_rate
    tone = np.exp(1j * phase)
    if sample_format == "CF32":
        return tone.astype(np.complex64)

    interleaved = np.empty(count * 2, dtype=np.int16)
    interleaved[0::2] = full_scale * tone.real
    interleaved[1::2] = full_scale * tone.imag
    return interleaved


def write_all(sdr, stream, samples, count, scalars_per_element):
    """Write all stream elements while accepting valid partial writes."""
    written = 0
    while written < count:
        start = written * scalars_per_element
        result = sdr.writeStream(
            stream,
            [samples[start:]],
            count - written,
        )
        if result.ret < 0:
            raise RuntimeError(
                f"writeStream failed: {SoapySDR.errToStr(result.ret)} ({result.ret})"
            )
        if result.ret == 0:
            raise RuntimeError("writeStream returned zero samples")
        written += result.ret


def transmit(args):
    """Configure a TX stream and transmit until interrupted or timed out."""
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

        stream_format = (
            SOAPY_SDR_CF32 if args.sample_format == "CF32" else SOAPY_SDR_CS16
        )
        stream = sdr.setupStream(SOAPY_SDR_TX, stream_format, [args.channel])
        mtu = sdr.getStreamMTU(stream)
        offset = args.offset if args.offset is not None else args.rate / 4.0
        full_scale = int(sdr.readSetting("full_scale"))
        samples = make_tone(mtu, args.sample_format, offset, args.rate, full_scale)
        scalars_per_element = 1 if args.sample_format == "CF32" else 2

        result = sdr.activateStream(stream)
        if result < 0:
            raise RuntimeError(
                f"activateStream failed: {SoapySDR.errToStr(result)} ({result})"
            )
        active = True

        print(
            f"Transmitting a {offset:,.0f} Hz tone in {args.sample_format}; "
            "press Ctrl-C to stop"
        )
        started = time.monotonic()
        next_progress = started + 1.0
        while args.duration == 0 or time.monotonic() - started < args.duration:
            write_all(sdr, stream, samples, mtu, scalars_per_element)
            now = time.monotonic()
            if now >= next_progress:
                print(".", end="", flush=True)
                next_progress = now + 1.0
        print()
    except KeyboardInterrupt:
        print("\nStopping transmission")
    finally:
        if stream is not None:
            if active:
                sdr.deactivateStream(stream)
            sdr.closeStream(stream)


def parse_arguments(argv):
    """Parse command-line options for the tone example."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--tx-block-size", type=int, default=16380)
    parser.add_argument("-r", "--rate", type=float, default=10e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("--offset", type=float)
    parser.add_argument("-a", "--attenuation", type=float, default=35)
    parser.add_argument(
        "--format", dest="sample_format", choices=("CS16", "CF32"), default="CS16"
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
    transmit(parse_arguments(sys.argv[1:]))
