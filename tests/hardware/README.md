# Hardware validation programs

These programs require a Sidekiq card and, in some cases, additional equipment
or configuration such as an external PPS source or an RF loopback path.

The bounded validation programs return a nonzero exit status on a failed
assertion and close streams on exit. Run them one at a time against a card with
a supported sample rate. The interactive `test_api.py` program is still a
legacy diagnostic pending comparison with the non-hardware API test suite.

Current programs:

- `cf32_validate.py` checks converted CF32 counter samples.
- `cs16_validate.py` checks CS16 counter samples and timestamps.
- `run_loop.py` repeatedly activates and deactivates an RX stream.
- `test_api.py` interactively exercises device-control APIs.

For example, on a card supporting 2 MS/s:

```sh
python3 tests/hardware/cs16_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/cf32_validate.py --serial 47479 --rate 2e6
python3 tests/hardware/run_loop.py --serial 47479 --rate 2e6
```

The counter tests use the FPGA's internal counter source, so an RF input is not
needed. `cs16_validate.py` checks both RF and system sample timestamps.

The bounded timestamp, simultaneous RX/TX, and RX retuning examples are in
[`examples/python`](../../examples/python/).
