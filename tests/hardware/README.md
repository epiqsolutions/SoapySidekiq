# Hardware validation programs

For the combined integration branch, follow the
[hardware integration test plan](INTEGRATION_TEST_PLAN.md) and record outcomes
in [INTEGRATION_VALIDATION.md](INTEGRATION_VALIDATION.md).

These programs require a Sidekiq card and, in some cases, additional equipment
or configuration such as an external PPS source or an RF loopback path.

The bounded validation programs return a nonzero exit status on a failed
assertion and close streams on exit. Run them one at a time against a card with
a supported sample rate. The interactive `test_api.py` program is still a
legacy diagnostic pending comparison with the non-hardware API test suite.

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
driver assertion. `test_api.py` also contains fixed settings and is a manual
diagnostic, not a model-independent pass/fail test.

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
  immediate cleanup after timed activation, and stream reuse.
- `test_api.py` interactively exercises device-control APIs.

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
