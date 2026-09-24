# Hardware validation programs

These programs require a Sidekiq card and, in some cases, additional equipment
or configuration such as an external PPS source or an RF loopback path.

They have not yet been converted into a common automated hardware-test
harness. Some retain legacy assumptions about exact `readStream()` sizes,
requests larger than the stream MTU, interactive input, or indefinite run
times. Do not treat a successful process exit as a complete validation result
until the individual program has been modernized.

Current programs:

- `cf32_validate.py` checks converted CF32 counter samples.
- `cs16_validate.py` checks CS16 counter samples and timestamps.
- `fdd_lo_samples.py` exercises simultaneous RX/TX while retuning RX.
- `get_offset_overload.py` monitors overload and calibration-offset settings.
- `run_cs16.py` exercises sustained CS16 receive throughput.
- `run_loop.py` repeatedly activates and deactivates an RX stream.
- `test_api.py` interactively exercises device-control APIs.

The bounded timestamp and simultaneous RX/TX examples are in
[`examples/python`](../../examples/python/).
