#!/usr/bin/env python3

"""Reset and print Sidekiq hardware timestamps at a fixed interval.

The RX sample rate is needed when reading the RX RF timestamp. Resetting time
also changes the card's time reference for other applications.
"""

import argparse
import sys
import time

import SoapySDR
from SoapySDR import SOAPY_SDR_RX


def show_timestamps(args):
    device_args = {"driver": "sidekiq"}
    device_args["serial" if args.serial else "card"] = args.serial or args.card
    if args.topology:
        device_args["topology"] = args.topology

    sdr = SoapySDR.Device(device_args)
    if args.source == "rx_rf_timestamp":
        sdr.setSampleRate(SOAPY_SDR_RX, args.channel, args.rate)
    sdr.setHardwareTime(0, "now")

    previous = None
    for index in range(args.count):
        current = sdr.getHardwareTime(args.source)
        delta = "first reading" if previous is None else f"delta {current - previous:,} ns"
        print(f"{index + 1}: {current:,} ns ({delta})")
        previous = current
        if index + 1 < args.count:
            time.sleep(args.interval)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-c", "--card", default="0", help="Sidekiq card number")
    parser.add_argument("--serial", help="Select a Sidekiq card by serial number")
    parser.add_argument("--topology", help="Sidekiq topology ID")
    parser.add_argument("--channel", type=int, default=0, help="RX channel")
    parser.add_argument(
        "--source",
        choices=("rx_rf_timestamp", "sys_timestamp"),
        default="rx_rf_timestamp",
    )
    parser.add_argument("--rate", type=float, default=2e6, help="RX sample rate")
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds between readings")
    args = parser.parse_args(argv)
    if args.count <= 0:
        parser.error("--count must be positive")
    if args.interval < 0:
        parser.error("--interval cannot be negative")
    if args.rate <= 0:
        parser.error("--rate must be positive")
    return args


if __name__ == "__main__":
    show_timestamps(parse_arguments(sys.argv[1:]))
