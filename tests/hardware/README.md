# Hardware validation programs

For the combined integration branch, follow the
[hardware integration test plan](INTEGRATION_TEST_PLAN.md). Save each run's
commands, card and RF setup, results, and capability skips with that run's
review or release evidence. The plan describes one integration node; check
this README and the selected cards' capabilities when using another node.

These programs require a Sidekiq card and, in some cases, additional equipment
or configuration such as an external PPS source or an RF loopback path.

The bounded validation programs return a nonzero exit status on a failed
assertion and close streams on exit. Run them one at a time against a card with
a supported sample rate. The `test_api.py` diagnostic checks capabilities and
passive readbacks without starting streams or requiring RF equipment.

## Card and setup limits

This is not an automatic suite for every Sidekiq model or card combination.
Select a discovered card and supported channel, sample rate, bandwidth, and
gain settings before running each program. The single-card counter checks do
not need an RF input, but paired tone, full-duplex, and retune checks need
compatible cards and a verified RF path. PPS checks need an available PPS
source. Record unsupported combinations as skips rather than test failures.

Several paired validators default to the `8N55`/`47479` configuration used on
the integration node, including a 2 MS/s rate, 2047 RX host full scale, and a
short startup timeout. Those defaults do not fit every card. For example,
NV100 uses a 1.92 MS/s nominal SDK profile, 32767 CS16 host full scale, and
can take more than 6 seconds to initialize. Supply the relevant arguments
explicitly and check the card's capabilities before treating a result as a
driver assertion. `test_api.py` is a noninteractive capability/readback
diagnostic described below; it does not apply those fixed RF settings.

Current programs:

- `cf32_validate.py` checks converted CF32 counter samples.
- `cs16_validate.py` checks CS16 counter samples and timestamps.
- `run_loop.py` repeatedly activates and deactivates an RX stream.
- `partial_read_validate.py` checks counter order and timestamp phase when each
  read requests less than one stream MTU.
- `paired_tone_validate.py` coordinates bounded TX/RX processes on two cards
  and asserts received tone frequency and level from a saved capture.
- `full_duplex_pair_validate.py` runs one process per card and checks distinct
  peer tones in both directions while each card transmits and receives.
- `paired_retune_validate.py` holds a selected peer TX active in a separate
  process while PCIe RX retunes and checks both captured tone frequencies.
- `pps_lifecycle_validate.py` checks external-PPS RX/TX timed starts and stops,
  immediate cleanup after timed activation, and stream reuse. Its first timed
  TX write retries per-call timeouts for up to five seconds while PPS startup
  is pending.
- `test_api.py` reports API capabilities and passive RF readbacks with explicit
  device selection, pass/fail/skip results, and an optional JSON report.

The full-duplex detector searches actual spectral peaks within the larger of
20 kHz or two FFT bins of the expected peer frequency. It requires the peak
to be at least 10 dB above the median spectral noise over a five-bin main-lobe
bandwidth and within 40 dB of the
strongest tone in that capture, rejecting weak spurs from unrelated tones.
`--min-peer-relative-db` adjusts the latter threshold for a measured RF setup;
record any change with the run. This is a relative detection threshold, not
an absolute RF power measurement. Synthetic regressions cover the wrong-tone
quantization-spur failure and a weaker real peer alongside local leakage.

## Automated API diagnostic

Run without an RF generator, analyzer, or PPS source:

```sh
python3 tests/hardware/test_api.py --serial 47479 --json api-result.json
python3 tests/hardware/test_api.py -c 0 --direction rx --channel 1 --json rx1-api.json
```

Choose exactly one card index (`-c`/`--cardno`) or `--serial`. By default, the
diagnostic checks every advertised RX and TX channel. `--channel` (also accepted
as `--handle`) is a repeatable **logical** channel index; an explicitly selected
channel must exist in each selected direction. A direction with no channels is
reported as skipped when checking the default channel set.

The checks cover identification, stream format/full-scale metadata, RF ranges,
current sample rate and bandwidth readbacks, sensor metadata, and clock/time-source
enumeration. The diagnostic does not read an LO frequency because it does not
configure one. Readback errors fail. Failures are collected while other checks
continue, and the exit status is nonzero if any check fails.
`--json` records details and errors even when device construction fails.
`--help` works without the SoapySDR bindings.

This replaces the old interactive script and its fixed write settings. It does
not prompt, call RF setters, or create/activate streams. Device construction
still performs normal driver/SDK initialization. If `--topology ID` is supplied,
that explicit topology is applied while constructing the device. The old
`-r`, `-bw`, `-f`, `-g`, `-s`, and `-d` options for fixed write/capture settings
are removed; use the dedicated streaming validators for those operations.

Sensor values are read only with `--read-sensors`; accelerometer reads can
enable/disable sampling. The driver's legacy `0`/`{}` sensor fallbacks are
recorded as returned values and do not establish successful or calibrated
physical measurements. Without that option only sensor metadata is queried.

A successful API report establishes neither RF accuracy nor streaming/PPS
behavior. Use the counter, partial-read, PPS, and paired-RF tests below for those
checks and retain the report with the card/SDK/build evidence. The new diagnostic
has software coverage using a fake device; a physical run remains required.

## Known integration limits

The driver currently supports one RX stream and one TX stream per device,
with one channel in each stream. An advertised handle does not establish that
every RX/TX handle pair can operate together. Confirm the SDK topology and RF
port mapping for the chosen pair.

On the integration node, the M.2-2280 (`8N55`) can crash during SDK
initialization if the PCIe card (`47479`) is already transmitting. The same
startup order failed with SDK prebuilt apps from versions 4.24–4.26, without
SoapySidekiq. Initialize `8N55` before starting PCIe TX for that pair.

The NV100 (`9YAA`) RX B1/J2 plus TX A1/J1 pair passed full duplex validation
at 1.92 MS/s. J2 TX did not produce a measurable tone in that node's RF
setup, including a direct SDK app check; its RF path remains unverified.
NV100's tested low, nominal, and high profiles were 1, 1.92, and 10 MS/s
with bandwidth set to 80% of the sample rate. A 2 MS/s request was rejected
by the SDK on this node.

The `8N55` high-output tone became distorted at some TX gain settings in the
tested RF path, including with direct SDK apps. Choose TX gain from measured
receiver level and waveform quality; an FFT peak alone is insufficient to
establish a clean tone. These observations do not establish calibrated RF
power or path attenuation.

For example, on a card supporting 2 MS/s:

```sh
python3 tests/hardware/cs16_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/cf32_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/run_loop.py --serial 47479 --rate 2e6
```

Replace `47479` and `2e6` with the selected card's serial and a supported
rate. Set `SOAPY_SDR_ROOT` to the intended module's install prefix when more
than one SoapySidekiq build is installed.

The counter tests use the FPGA's internal counter source, so an RF input is not
needed. `cs16_validate.py` checks both RF and system sample timestamps.

The bounded timestamp, simultaneous RX/TX, and RX retuning examples are in
[`examples/python`](../../examples/python/).
