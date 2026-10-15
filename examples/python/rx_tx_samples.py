#!/usr/bin/env python3

"""Transmit a tone while recording CS16 samples from the same Sidekiq card.

Connect TX to RX through a suitable attenuator or approved lab path before
running this example. The output file contains little-endian interleaved I/Q.
"""

import argparse
import sys
import threading
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT, SOAPY_SDR_TX

from tx_tone import make_tone, write_all


def device_arguments(args):
    result = {"driver": "sidekiq", "tx_block_size": str(args.tx_block_size)}
    result["serial" if args.serial else "card"] = args.serial or args.card
    if args.topology:
        result["topology"] = args.topology
    return result


def capture_while_transmitting(args):
    sdr = SoapySDR.Device(device_arguments(args))
    rx_stream = None
    tx_stream = None
    rx_active = False
    stop_tx = threading.Event()
    tx_errors = []
    tx_samples = 0
    tx_thread = None

    try:
        for direction, channel in ((SOAPY_SDR_RX, args.rx_channel),
                                   (SOAPY_SDR_TX, args.tx_channel)):
            sdr.setSampleRate(direction, channel, args.rate)
            sdr.setBandwidth(
                direction, channel,
                args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
            )
            sdr.setFrequency(direction, channel, args.frequency)

        if args.rx_gain is None:
            sdr.setGainMode(SOAPY_SDR_RX, args.rx_channel, True)
        else:
            sdr.setGainMode(SOAPY_SDR_RX, args.rx_channel, False)
            sdr.setGain(SOAPY_SDR_RX, args.rx_channel, args.rx_gain)
        sdr.setGain(SOAPY_SDR_TX, args.tx_channel, args.attenuation)

        rx_stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [args.rx_channel])
        tx_stream = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, [args.tx_channel])
        rx_mtu = sdr.getStreamMTU(rx_stream)
        tx_mtu = sdr.getStreamMTU(tx_stream)
        rx_buffer = np.empty(rx_mtu * 2, dtype="<i2")
        offset = args.offset if args.offset is not None else args.rate / 4.0
        full_scale = int(sdr.readSetting("full_scale"))
        tone = make_tone(tx_mtu, "CS16", offset, args.rate, full_scale)

        def transmit():
            nonlocal tx_samples
            active = False
            try:
                result = sdr.activateStream(tx_stream)
                if result < 0:
                    raise RuntimeError(f"TX activateStream failed: {result}")
                active = True
                while not stop_tx.is_set():
                    write_all(sdr, tx_stream, tone, tx_mtu, 2)
                    tx_samples += tx_mtu
            except Exception as error:
                tx_errors.append(error)
            finally:
                if active:
                    try:
                        sdr.deactivateStream(tx_stream)
                    except Exception as error:
                        tx_errors.append(error)
                stop_tx.set()

        received = 0
        with open(args.output, "wb") as output:
            result = sdr.activateStream(rx_stream)
            if result < 0:
                raise RuntimeError(f"RX activateStream failed: {result}")
            rx_active = True
            tx_thread = threading.Thread(target=transmit, name="sidekiq-tx")
            tx_thread.start()

            print(f"Transmitting a {offset:,.0f} Hz tone and recording to {args.output}")
            deadline = time.monotonic() + args.duration
            while not stop_tx.is_set() and time.monotonic() < deadline:
                result = sdr.readStream(
                    rx_stream, [rx_buffer], rx_mtu, timeoutUs=args.timeout_us,
                )
                if result.ret == SOAPY_SDR_TIMEOUT or result.ret == 0:
                    continue
                if result.ret < 0:
                    raise RuntimeError(
                        f"readStream failed: {SoapySDR.errToStr(result.ret)} "
                        f"({result.ret})"
                    )
                output.write(rx_buffer[: result.ret * 2].tobytes())
                received += result.ret

    finally:
        stop_tx.set()
        if tx_thread is not None:
            tx_thread.join()
        try:
            if rx_active:
                sdr.deactivateStream(rx_stream)
        finally:
            try:
                if tx_stream is not None:
                    sdr.closeStream(tx_stream)
            finally:
                if rx_stream is not None:
                    sdr.closeStream(rx_stream)

    if tx_errors:
        raise RuntimeError("TX stream failed") from tx_errors[0]
    if received == 0 or tx_samples == 0:
        raise RuntimeError("No RX or TX samples were processed")
    print(f"Received {received:,} samples ({received * 4:,} bytes)")
    print(f"Transmitted {tx_samples:,} samples")


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="rx_tx_samples.bin")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--rx-channel", type=int, default=0)
    parser.add_argument("--tx-channel", type=int, default=0)
    parser.add_argument("--tx-block-size", type=int, default=16380)
    parser.add_argument("-r", "--rate", type=float, default=2e6)
    parser.add_argument("-b", "--bandwidth", type=float)
    parser.add_argument("-f", "--frequency", type=float, default=1e9)
    parser.add_argument("--offset", type=float, help="Tone offset above the LO")
    parser.add_argument("--rx-gain", type=float, help="RX gain; default is automatic")
    parser.add_argument("-a", "--attenuation", type=float, default=0)
    parser.add_argument("--duration", type=float, default=3, help="Capture duration in seconds")
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    if args.duration <= 0:
        parser.error("--duration must be positive")
    if args.rate <= 0:
        parser.error("--rate must be positive")
    if args.tx_block_size <= 0:
        parser.error("--tx-block-size must be positive")
    if args.timeout_us <= 0:
        parser.error("--timeout-us must be positive")
    return args


if __name__ == "__main__":
    capture_while_transmitting(parse_arguments(sys.argv[1:]))
