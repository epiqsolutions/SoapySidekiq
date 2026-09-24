#!/usr/bin/env python3

"""Retune RX between short captures while a tone continues transmitting.

Connect TX to RX through a suitable attenuator or approved lab path. Each
capture is written as little-endian interleaved CS16 I/Q samples. The RX stream
is stopped before each retune; the TX stream stays active throughout.
"""

import argparse
import sys
import threading
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT, SOAPY_SDR_TX

from tx_tone import make_tone, write_all


def retune_while_transmitting(args):
    device_args = {"driver": "sidekiq", "tx_block_size": str(args.tx_block_size)}
    device_args["serial" if args.serial else "card"] = args.serial or args.card
    if args.topology:
        device_args["topology"] = args.topology

    sdr = SoapySDR.Device(device_args)
    rx_stream = None
    tx_stream = None
    rx_active = False
    tx_thread = None
    stop_tx = threading.Event()
    tx_errors = []
    tx_samples = 0

    try:
        for direction, channel in ((SOAPY_SDR_RX, args.rx_channel),
                                   (SOAPY_SDR_TX, args.tx_channel)):
            sdr.setSampleRate(direction, channel, args.rate)
            sdr.setBandwidth(
                direction, channel,
                args.bandwidth if args.bandwidth is not None else args.rate * 0.8,
            )
        sdr.setFrequency(SOAPY_SDR_TX, args.tx_channel, args.tx_frequency)
        sdr.setFrequency(
            SOAPY_SDR_RX, args.rx_channel, args.tx_frequency + args.rx_offsets[0]
        )
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
        tone_offset = args.tone_offset if args.tone_offset is not None else args.rate / 4
        tone = make_tone(
            tx_mtu, "CS16", tone_offset, args.rate,
            int(sdr.readSetting("full_scale")),
        )

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

        tx_thread = threading.Thread(target=transmit, name="sidekiq-tx")
        tx_thread.start()

        for index, rx_offset in enumerate(args.rx_offsets, start=1):
            if stop_tx.is_set():
                break
            if index > 1:
                sdr.deactivateStream(rx_stream)
                rx_active = False
            rx_frequency = args.tx_frequency + rx_offset
            if index > 1:
                sdr.setFrequency(SOAPY_SDR_RX, args.rx_channel, rx_frequency)
            result = sdr.activateStream(rx_stream)
            if result < 0:
                raise RuntimeError(f"RX activateStream failed: {result}")
            rx_active = True

            output_path = f"{args.output_prefix}_{index}.cs16"
            received = 0
            deadline = time.monotonic() + args.seconds_per_frequency
            with open(output_path, "wb") as output:
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

            if received == 0:
                raise RuntimeError(f"No RX samples at {rx_frequency:,.0f} Hz")
            print(
                f"RX LO {rx_frequency:,.0f} Hz: {received:,} samples to {output_path}; "
                f"expected tone offset {tone_offset - rx_offset:+,.0f} Hz"
            )
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
    if tx_samples == 0:
        raise RuntimeError("No TX samples were sent")
    print(f"Transmitted {tx_samples:,} samples across {len(args.rx_offsets)} RX frequencies")


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_prefix", nargs="?", default="rx_retune")
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--rx-channel", type=int, default=0)
    parser.add_argument("--tx-channel", type=int, default=0)
    parser.add_argument("--tx-block-size", type=int, default=16380)
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--bandwidth", type=float)
    parser.add_argument("--tx-frequency", type=float, default=1e9)
    parser.add_argument("--tone-offset", type=float, help="Tone frequency above TX LO")
    parser.add_argument(
        "--rx-offsets", default="0,1e6",
        help="Comma-separated RX LO offsets from TX LO in Hz",
    )
    parser.add_argument("--rx-gain", type=float, help="RX gain; default is automatic")
    parser.add_argument("--attenuation", type=float, default=0)
    parser.add_argument("--seconds-per-frequency", type=float, default=1)
    parser.add_argument("--timeout-us", type=int, default=200000)
    args = parser.parse_args(argv)
    try:
        args.rx_offsets = [float(item) for item in args.rx_offsets.split(",")]
    except ValueError:
        parser.error("--rx-offsets must be comma-separated numbers")
    if not args.rx_offsets or any(not np.isfinite(item) for item in args.rx_offsets):
        parser.error("--rx-offsets must contain finite numbers")
    if args.rate <= 0 or not np.isfinite(args.rate):
        parser.error("--rate must be positive and finite")
    if args.seconds_per_frequency <= 0:
        parser.error("--seconds-per-frequency must be positive")
    if args.tx_block_size <= 0 or args.timeout_us <= 0:
        parser.error("--tx-block-size and --timeout-us must be positive")
    return args


if __name__ == "__main__":
    retune_while_transmitting(parse_arguments(sys.argv[1:]))
