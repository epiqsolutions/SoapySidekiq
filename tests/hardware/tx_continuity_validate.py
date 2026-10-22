#!/usr/bin/env python3
"""Observe TX envelope continuity while the same card stops/retunes/restarts RX.

Requires an attenuated DUT TX -> independent observer RX path. Immediate starts
only. Use an outer process timeout for SDK calls that may block during cleanup.
"""

import argparse
import json
import math
import multiprocessing as mp
from pathlib import Path
import threading
import time

import numpy as np


def require_ok(label, status):
    if status < 0:
        raise RuntimeError(f"{label}: {status}")


def timestamp_step(previous, timestamp, count, rate):
    """Reject missing/repeated observer samples; allow one sample of rounding."""
    if count <= 0:
        raise AssertionError("observer returned no samples")
    if previous is not None:
        last_time, last_count = previous
        expected = last_time + last_count * 1e9 / rate
        if abs(timestamp - expected) > max(2, 1e9 / rate):
            raise AssertionError(f"observer timestamp discontinuity: {timestamp} vs {expected}")
    return timestamp, count


def measure_signal(path, rate, full_scale, offset, max_gap_us=100,
                   drop_db=12, min_baseline_fs=0.005):
    """Inspect every envelope sample and successive FFT windows, not a midpoint.

    A pass bounds below-threshold envelope gaps at the stated resolution. It
    does not establish sample identity, perfect phase, or SDK shutdown barriers.
    """
    if (not all(math.isfinite(v) for v in
                (rate, full_scale, offset, max_gap_us, drop_db, min_baseline_fs))
            or min(rate, full_scale, max_gap_us, drop_db, min_baseline_fs) <= 0
            or abs(offset) >= rate / 2):
        raise ValueError("invalid continuity measurement parameters")
    if Path(path).stat().st_size % 4:
        raise AssertionError("truncated complex sample")
    raw = np.memmap(path, dtype="<i2", mode="r")
    if raw.size < 32768:
        raise AssertionError("capture too short")
    iq = raw.reshape(-1, 2)
    baseline = iq[:min(len(iq), max(16384, int(rate * 0.2)))].astype(float)
    reference = float(np.median(np.hypot(baseline[:, 0], baseline[:, 1])))
    if reference / full_scale < min_baseline_fs:
        raise AssertionError("baseline RF level too low")
    threshold = reference * 10 ** (-drop_db / 20)
    longest = carry = 0
    minimum_tone_db = math.inf
    window = 16384
    for start in range(0, len(iq), window):
        block = iq[start:start + window].astype(float)
        if np.max(np.abs(block)) >= full_scale * 0.8:
            raise AssertionError("observer level too high or clipped")
        low = np.hypot(block[:, 0], block[:, 1]) < threshold
        # Include an unfinished run from the preceding chunk.
        edges = np.flatnonzero(np.diff(np.r_[False, low, False].astype(np.int8)))
        for first, end in edges.reshape(-1, 2):
            length = int(end - first) + (carry if first == 0 else 0)
            longest = max(longest, length)
        carry = (int(len(low) - edges[-2]) + (carry if edges[-2] == 0 else 0)
                 if low[-1] else 0)
        if longest * 1e6 / rate > max_gap_us:
            raise AssertionError(f"RF envelope gap exceeds {max_gap_us:g} us")
        # The final partial window's envelope is still checked above.
        if len(block) < window:
            block = iq[-window:].astype(float)
        samples = block[:, 0] + 1j * block[:, 1]
        samples -= np.mean(samples)
        spectrum = np.abs(np.fft.fft(samples * np.hanning(window)))
        peak = int(np.argmax(spectrum))
        peak_hz = float(np.fft.fftfreq(window, 1 / rate)[peak])
        tone_db = 20 * math.log10(max(float(spectrum[peak]), 1e-12)
                                 / max(float(np.median(spectrum)), 1e-12))
        minimum_tone_db = min(minimum_tone_db, tone_db)
        if abs(peak_hz - offset) > max(20000, 2 * rate / window) or tone_db < 10:
            raise AssertionError("expected tone absent in an observation window")
    return {"samples": len(iq), "duration_s": len(iq) / rate,
            "baseline_fs": reference / full_scale, "drop_db": drop_db,
            "max_gap_us": max_gap_us, "longest_low_us": longest * 1e6 / rate,
            "minimum_tone_noise_db": minimum_tone_db}


def validate_events(events, cycles):
    """Require a complete measured interval surrounding every requested retune."""
    expected = ["ready"] + [phase for _ in range(cycles)
                            for phase in ("retune_begin", "retune_end")] + ["done"]
    if [event["phase"] for event in events] != expected:
        raise AssertionError("incomplete retune event sequence")
    positions = [event["observer_samples"] for event in events]
    if any(right <= left for left, right in zip(positions, positions[1:])):
        raise AssertionError("observer capture does not span every retune phase")
    for cycle in range(cycles):
        begin, end = events[1 + 2 * cycle:3 + 2 * cycle]
        if (begin["cycle"] != cycle or end["cycle"] != cycle
                or end["accepted"] <= begin["accepted"]):
            raise AssertionError("missing TX progress in retune cycle")


def dut_worker(args, connection):
    """One device instance owns concurrent TX writes and the RX lifecycle."""
    import SoapySDR as S
    device = S.Device({"driver": "sidekiq", "serial": args.dut_serial,
                       "tx_buffer_count": str(args.tx_buffer_count), "tx_block_size": "16380"})
    rx = tx = None
    rx_active = tx_active = False
    stop = threading.Event()
    first_write = threading.Event()
    errors = []
    thread = None
    accepted = 0
    try:
        device.writeSetting("iq_swap", "false")
        device.writeSetting("counter", "false")
        for direction, channel in ((S.SOAPY_SDR_RX, args.dut_rx_channel),
                                   (S.SOAPY_SDR_TX, args.dut_tx_channel)):
            device.setSampleRate(direction, channel, args.rate)
            device.setBandwidth(direction, channel, args.rate * 0.8)
            device.setFrequency(direction, channel, args.frequency)
        device.setGainMode(S.SOAPY_SDR_RX, args.dut_rx_channel, False)
        device.setGain(S.SOAPY_SDR_RX, args.dut_rx_channel, args.dut_rx_gain)
        device.setGain(S.SOAPY_SDR_TX, args.dut_tx_channel, args.attenuation)
        rx = device.setupStream(S.SOAPY_SDR_RX, S.SOAPY_SDR_CS16, [args.dut_rx_channel])
        tx = device.setupStream(S.SOAPY_SDR_TX, S.SOAPY_SDR_CS16, [args.dut_tx_channel])
        mtu = device.getStreamMTU(tx)
        rate = device.getSampleRate(S.SOAPY_SDR_TX, args.dut_tx_channel)
        # An integer number of cycles makes repeated whole-MTU buffers coherent.
        offset = round(args.offset * mtu / rate) * rate / mtu
        scale = device.getNativeStreamFormat(S.SOAPY_SDR_TX, args.dut_tx_channel).fullScale
        tone = 0.25 * scale * np.exp(2j * np.pi * offset * np.arange(mtu) / rate)
        buffer = np.column_stack((tone.real, tone.imag)).astype(np.int16).ravel()
        require_ok("TX activate", device.activateStream(tx))
        tx_active = True

        def transmit():
            nonlocal accepted
            try:
                while not stop.is_set():
                    result = device.writeStream(tx, [buffer], mtu, timeoutUs=200000)
                    if result.ret != mtu:
                        raise RuntimeError(f"TX accepted {result.ret} of {mtu}")
                    accepted += result.ret
                    first_write.set()
            except Exception as error:
                errors.append(str(error))
                first_write.set()

        thread = threading.Thread(target=transmit)
        thread.start()
        if not first_write.wait(5) or errors:
            raise RuntimeError(f"TX did not start: {errors}")
        rx_buffer = np.empty(device.getStreamMTU(rx) * 2, dtype="<i2")
        require_ok("RX activate", device.activateStream(rx))
        rx_active = True

        def dwell():
            end = time.monotonic() + args.dwell
            count = 0
            while time.monotonic() < end:
                if errors:
                    raise RuntimeError(f"TX writer failed: {errors}")
                result = device.readStream(rx, [rx_buffer], len(rx_buffer) // 2,
                                           timeoutUs=200000)
                require_ok("DUT RX read", result.ret)
                count += result.ret
            if not count:
                raise AssertionError("DUT RX returned no samples")

        dwell()  # Warm up before the observer starts its measurement interval.
        connection.send({"phase": "ready", "offset": offset, "tx_rate": rate,
                         "tx_full_scale": scale, "rx_rate": device.getSampleRate(
                             S.SOAPY_SDR_RX, args.dut_rx_channel)})
        if not connection.poll(5) or connection.recv() != "capture":
            raise TimeoutError("observer did not acknowledge capture")
        dwell()  # Stable baseline before the first retune.
        for cycle in range(args.cycles):
            before = accepted
            connection.send({"phase": "retune_begin", "cycle": cycle,
                             "monotonic": time.monotonic(), "accepted": before})
            require_ok("RX stop", device.deactivateStream(rx))
            rx_active = False
            frequency = args.frequency + (args.retune_hz if cycle % 2 == 0 else 0)
            device.setFrequency(S.SOAPY_SDR_RX, args.dut_rx_channel, frequency)
            require_ok("RX restart", device.activateStream(rx))
            rx_active = True
            dwell()
            if accepted <= before:
                raise AssertionError("TX accepted no new samples across RX retune")
            actual_frequency = device.getFrequency(S.SOAPY_SDR_RX, args.dut_rx_channel)
            if abs(actual_frequency - frequency) > 1:
                raise AssertionError("RX retune readback differs from requested frequency")
            connection.send({"phase": "retune_end", "cycle": cycle,
                             "monotonic": time.monotonic(), "accepted": accepted,
                             "rx_frequency": actual_frequency})
        dwell()
        connection.send({"phase": "done", "accepted": accepted})
        if not connection.poll(args.deadline) or connection.recv() != "stop":
            raise TimeoutError("observer did not finish capture")
    finally:
        stop.set()
        if thread is not None:
            thread.join()
        try:
            if rx_active:
                require_ok("RX final stop", device.deactivateStream(rx))
        finally:
            try:
                if tx_active:
                    require_ok("TX final stop", device.deactivateStream(tx))
            finally:
                try:
                    if tx is not None:
                        device.closeStream(tx)
                finally:
                    if rx is not None:
                        device.closeStream(rx)
    if errors:
        raise RuntimeError(f"TX writer failed: {errors}")


def validate(args):
    import SoapySDR as S
    directory = Path(args.output_dir)
    directory.mkdir(parents=True, exist_ok=False)
    report = {"arguments": vars(args), "status": "FAIL", "events": []}
    stream = process = None
    active = False
    try:
        device = S.Device({"driver": "sidekiq", "serial": args.observer_serial,
                           "tx_buffer_count": "1"})
        device.writeSetting("counter", "false")
        device.writeSetting("iq_swap", "false")
        device.writeSetting("timetype", "rf_timestamp")
        device.setSampleRate(S.SOAPY_SDR_RX, args.observer_channel, args.rate)
        device.setBandwidth(S.SOAPY_SDR_RX, args.observer_channel, args.rate * 0.8)
        device.setFrequency(S.SOAPY_SDR_RX, args.observer_channel, args.frequency)
        device.setGainMode(S.SOAPY_SDR_RX, args.observer_channel, False)
        device.setGain(S.SOAPY_SDR_RX, args.observer_channel, args.observer_gain)
        rate = device.getSampleRate(S.SOAPY_SDR_RX, args.observer_channel)
        scale = device.getNativeStreamFormat(S.SOAPY_SDR_RX, args.observer_channel).fullScale
        report["observer"] = {"rate": rate, "full_scale": scale,
                              "hardware_info": dict(device.getHardwareInfo())}
        stream = device.setupStream(S.SOAPY_SDR_RX, S.SOAPY_SDR_CS16,
                                    [args.observer_channel])
        mtu = device.getStreamMTU(stream)
        buffer = np.empty(mtu * 2, dtype="<i2")
        context = mp.get_context("spawn")
        parent, child = context.Pipe()
        process = context.Process(target=dut_worker, args=(args, child))
        process.start()
        child.close()
        capturing = False
        previous = None
        done_target = None
        acknowledge_capture = False
        samples = 0
        deadline = time.monotonic() + args.deadline
        with (directory / "observer.cs16").open("wb") as output, \
                (directory / "blocks.jsonl").open("w") as blocks:
            while True:
                if time.monotonic() > deadline:
                    raise TimeoutError("continuity run deadline expired")
                if parent.poll():
                    event = parent.recv()
                    event["observer_samples"] = samples
                    report["events"].append(event)
                    if event["phase"] == "ready":
                        offset = event["offset"]
                        # Start a fresh observer session only after TX warms up.
                        # The DUT waits for the first captured block before retuning.
                        observer_start = time.monotonic()
                        require_ok("observer activate", device.activateStream(stream))
                        active = True
                        capturing = True
                        acknowledge_capture = True
                    elif event["phase"] == "done":
                        # Starting from a fresh session, this sample count covers
                        # elapsed wall time plus a tail even if delivery is queued.
                        done_target = math.ceil((time.monotonic() - observer_start
                                                 + args.dwell) * rate)
                if not process.is_alive():
                    raise RuntimeError(f"DUT worker exited early: {process.exitcode}")
                if not capturing:
                    time.sleep(0.001)
                    continue
                result = device.readStream(stream, [buffer], mtu, timeoutUs=200000)
                require_ok("observer read", result.ret)
                if not result.flags & S.SOAPY_SDR_HAS_TIME:
                    raise AssertionError("observer block has no timestamp")
                previous = timestamp_step(previous, result.timeNs, result.ret, rate)
                output.write(buffer[:result.ret * 2].tobytes())
                blocks.write(json.dumps({"index": samples, "count": result.ret,
                                         "time_ns": result.timeNs}) + "\n")
                samples += result.ret
                if acknowledge_capture:
                    parent.send("capture")
                    acknowledge_capture = False
                if done_target is not None and samples >= done_target:
                    parent.send("stop")
                    break
        report["observer"]["required_samples_through_tail"] = done_target
        process.join(timeout=5)
        if process.is_alive() or process.exitcode != 0:
            raise RuntimeError(f"DUT cleanup did not succeed: {process.exitcode}")
        validate_events(report["events"], args.cycles)
        report["measurement"] = measure_signal(directory / "observer.cs16", rate,
                                               scale, offset, args.max_gap_us,
                                               args.drop_db, args.min_baseline_fs)
        report["status"] = "PASS"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if process is not None and process.is_alive():
            process.terminate()
            process.join(timeout=2)
            if process.is_alive():
                process.kill()
                process.join(timeout=2)
        try:
            try:
                if active:
                    require_ok("observer stop", device.deactivateStream(stream))
            finally:
                if stream is not None:
                    device.closeStream(stream)
        except Exception as error:
            report["status"] = "FAIL"
            report["cleanup_error"] = str(error)
            raise
        finally:
            (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dut-serial", required=True)
    parser.add_argument("--observer-serial", required=True)
    for name in ("dut-rx-channel", "dut-tx-channel", "observer-channel"):
        parser.add_argument("--" + name, type=int, default=0)
    for name, default in (("rate", 2e6), ("frequency", 1e9), ("offset", 250e3),
                          ("retune-hz", 100e3), ("dwell", 0.5), ("deadline", 30),
                          ("attenuation", 80), ("dut-rx-gain", 0),
                          ("observer-gain", 0), ("max-gap-us", 100),
                          ("drop-db", 12), ("min-baseline-fs", 0.005)):
        parser.add_argument("--" + name, type=float, default=default)
    parser.add_argument("--cycles", type=int, default=4)
    parser.add_argument("--tx-buffer-count", type=int, default=1024)
    parser.add_argument("--output-dir", required=True, help="new directory for this run")
    args = parser.parse_args(argv)
    numbers = [v for v in vars(args).values() if isinstance(v, (int, float))]
    if (not all(math.isfinite(v) for v in numbers)
            or min(args.rate, args.frequency, args.dwell, args.deadline,
                   args.max_gap_us, args.drop_db, args.min_baseline_fs, args.cycles) <= 0
            or not 1 <= args.tx_buffer_count <= 30000
            or min(args.dut_rx_channel, args.dut_tx_channel, args.observer_channel) < 0
            or not args.dut_serial.strip() or not args.observer_serial.strip()
            or args.dut_serial == args.observer_serial or args.retune_hz == 0
            or args.frequency + args.retune_hz <= 0
            or not 0 < abs(args.offset) < args.rate / 2):
        parser.error("invalid channels, distinct serials, finite thresholds or timing")
    return args


if __name__ == "__main__":
    validate(parse_args())
