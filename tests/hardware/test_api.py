#!/usr/bin/env python3
"""Report Sidekiq API capabilities/readbacks without retuning or starting streams.

Device construction still performs the driver's normal hardware initialization.
An explicitly supplied topology is applied during construction. Sensor values
are read only with --read-sensors; accelerometer reads enable/disable sampling.
"""

import argparse
import json
import math
from pathlib import Path
import sys


def range_info(value):
    """Serialize a SoapySDR Range without assuming a particular card's limits."""
    result = {name: float(getattr(value, name)()) for name in ("minimum", "maximum", "step")}
    if not all(math.isfinite(number) for number in result.values()):
        raise ValueError("range contains a non-finite value")
    if result["minimum"] > result["maximum"] or result["step"] < 0:
        raise ValueError("range has inverted bounds or a negative step")
    return result


def argument_info(value):
    """Serialize the metadata shared by sensor and stream argument APIs."""
    return {name: str(getattr(value, name))
            for name in ("key", "name", "description", "units", "type")}


def collect_checks(device, directions, channels=None, read_sensors=False):
    """Collect independent pass/fail/skip records; never call setters or stream APIs.

    directions contains (display name, Soapy direction constant) pairs. Selected
    logical channels must exist in each requested direction; by default each
    direction checks all its advertised channels independently.
    """
    checks = []

    def check(name, operation):
        try:
            details = operation()
        except Exception as error:
            checks.append({"name": name, "status": "FAIL", "error": str(error)})
            return None
        checks.append({"name": name, "status": "PASS", "details": details})
        return details

    def identity():
        driver, hardware = device.getDriverKey(), device.getHardwareKey()
        if not driver or not hardware:
            raise ValueError("driver/hardware key is empty")
        return {"driver": driver, "hardware": hardware, "info": dict(device.getHardwareInfo())}

    def stream_metadata(direction, channel):
        formats = list(device.getStreamFormats(direction, channel))
        native, scale = device.getNativeStreamFormat(direction, channel)
        scale = float(scale)
        if native not in formats or not math.isfinite(scale) or scale <= 0:
            raise ValueError("native format/full scale disagrees with the advertised formats")
        return {"formats": formats, "native": native, "full_scale": scale,
                "arguments": [argument_info(arg)
                              for arg in device.getStreamArgsInfo(direction, channel)]}

    def rf_metadata(direction, channel):
        # Record capabilities; do not assume a continuous or exact profile list.
        return {
            "antennas": list(device.listAntennas(direction, channel)),
            "gain_names": list(device.listGains(direction, channel)),
            "gain_range": range_info(device.getGainRange(direction, channel)),
            "automatic_gain": bool(device.hasGainMode(direction, channel)),
            "frequency_ranges": [range_info(value)
                                 for value in device.getFrequencyRange(direction, channel)],
            "sample_rate_ranges": [range_info(value)
                                   for value in device.getSampleRateRange(direction, channel)],
        }

    def rf_readback(direction, channel):
        values = {"frequency": float(device.getFrequency(direction, channel)),
                  "sample_rate": float(device.getSampleRate(direction, channel)),
                  "bandwidth": float(device.getBandwidth(direction, channel))}
        if not all(math.isfinite(value) and value >= 0 for value in values.values()):
            raise ValueError("RF readback contains a negative or non-finite value")
        if values["sample_rate"] == 0 or values["bandwidth"] == 0:
            raise ValueError("actual sample rate/bandwidth is zero")
        return values

    check("identification", identity)
    for name, direction in directions:
        def channel_count():
            count = device.getNumChannels(direction)
            if count < 0:
                raise ValueError("negative channel count")
            return int(count)

        count = check(f"{name} channel count", channel_count)
        if count is None:
            continue
        if count == 0 and channels is None:
            checks.append({"name": f"{name} channels", "status": "SKIP",
                           "reason": "direction advertises no channels"})
        selected = range(count) if channels is None else channels
        for channel in selected:
            prefix = f"{name} channel {channel}"
            if channel < 0 or channel >= count:
                checks.append({"name": prefix, "status": "FAIL",
                               "error": f"requested channel is outside [0, {count})"})
                continue
            check(f"{prefix} stream metadata", lambda: stream_metadata(direction, channel))
            check(f"{prefix} RF metadata", lambda: rf_metadata(direction, channel))
            check(f"{prefix} RF readback", lambda: rf_readback(direction, channel))

    sensors = check("sensor enumeration", lambda: list(device.listSensors()))
    if sensors is not None:
        for key in sensors:
            def sensor_metadata():
                info = argument_info(device.getSensorInfo(key))
                if info["key"] != key:
                    raise ValueError("sensor metadata key differs from the advertised key")
                return info
            check(f"sensor {key} metadata", sensor_metadata)
            if read_sensors:
                check(f"sensor {key} readback", lambda: str(device.readSensor(key)))

    # Enumeration does not require a reference signal or an observed PPS edge.
    check("clock sources", lambda: list(device.listClockSources()))
    check("time sources", lambda: list(device.listTimeSources()))
    summary = {status.lower(): sum(item["status"] == status for item in checks)
               for status in ("PASS", "FAIL", "SKIP")}
    return {"checks": checks, "summary": summary}


def bounded_integer(low, high):
    """Build an argparse converter for a hardware identifier or logical channel."""
    def convert(text):
        try:
            value = int(text)
        except ValueError as error:
            raise argparse.ArgumentTypeError("expected an integer") from error
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"must be in [{low}, {high}]")
        return value
    return convert


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("-c", "--cardno", type=bounded_integer(0, 255), help="SDK card index")
    selector.add_argument("--serial", help="exact hardware serial")
    parser.add_argument("--topology", type=bounded_integer(0, 255),
                        help="explicit SDK topology applied when constructing the device")
    parser.add_argument("--direction", choices=("rx", "tx", "both"), default="both")
    parser.add_argument("--channel", "--handle", action="append", type=bounded_integer(0, 255),
                        help="logical channel to check (repeatable); default: all advertised channels")
    parser.add_argument("--read-sensors", action="store_true",
                        help="also read values; accelerometer reads enable/disable its sampling")
    parser.add_argument("--json", type=Path, help="write a machine-readable result file")
    args = parser.parse_args(argv)
    if args.serial is not None and not args.serial.strip():
        parser.error("serial must not be empty")
    return args


def main(argv=None):
    args = parse_args(argv)
    device_args = {"driver": "sidekiq"}
    device_args["serial" if args.serial is not None else "card"] = (
        args.serial if args.serial is not None else str(args.cardno))
    if args.topology is not None:
        device_args["topology"] = str(args.topology)
    stage = "device construction"
    try:
        # Keep --help usable on hosts without the Python SoapySDR bindings.
        import SoapySDR
        device = SoapySDR.Device(device_args)
        stage = "API diagnostic"
        directions = []
        if args.direction in ("rx", "both"):
            directions.append(("RX", SoapySDR.SOAPY_SDR_RX))
        if args.direction in ("tx", "both"):
            directions.append(("TX", SoapySDR.SOAPY_SDR_TX))
        report = collect_checks(device, directions, args.channel, args.read_sensors)
    except Exception as error:
        report = {"checks": [{"name": stage, "status": "FAIL", "error": str(error)}],
                  "summary": {"pass": 0, "fail": 1, "skip": 0}}
    report["device_args"] = device_args
    for item in report["checks"]:
        detail = item.get("error", item.get("reason", ""))
        print(f"[{item['status']}] {item['name']}" + (f": {detail}" if detail else ""))
    summary = report["summary"]
    print(f"{summary['pass']} passed, {summary['fail']} failed, {summary['skip']} skipped")
    if args.json is not None:
        try:
            args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        except OSError as error:
            print(f"Cannot write report {args.json}: {error}", file=sys.stderr)
            return 1
    return int(summary["fail"] != 0)


if __name__ == "__main__":
    sys.exit(main())
