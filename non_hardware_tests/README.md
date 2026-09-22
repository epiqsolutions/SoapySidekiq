# Non-hardware tests

These tests are separate from the hardware validation programs in `tests/`.
They do not open a Sidekiq card or link to the Sidekiq SDK. Test output from
the repository workflow is written to `out.txt`, which is intentionally
ignored by Git.

The suite has two layers:

- `unit/` tests isolated driver-support components.
- `component/` tests interactions with deterministic fake asynchronous devices.

Configure and run the normal suite:

```sh
cmake -S non_hardware_tests -B build/non_hardware
cmake --build build/non_hardware --parallel
ctest --test-dir build/non_hardware --output-on-failure
```

To watch a run, start this command in another terminal before launching the
suite:

```sh
tail -F out.txt
```

Then launch the repository runner:

```sh
non_hardware_tests/run_suite.sh
```

The runner writes verbose output to `out.txt` and appends an aggregate summary
in the form `Non-hardware test cases: <total> total, <passed> passed, <failed>
failed`. This counts the individual cases inside both executables; CTest's own
summary counts the two aggregate executables instead.

Run only one layer with CTest labels:

```sh
ctest --test-dir build/non_hardware -L unit --output-on-failure
ctest --test-dir build/non_hardware -L component --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S non_hardware_tests -B build/non_hardware-asan \
    -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel
ctest --test-dir build/non_hardware-asan --output-on-failure
```

ThreadSanitizer:

```sh
cmake -S non_hardware_tests -B build/non_hardware-tsan \
    -DSOAPYSIDEKIQ_TEST_TSAN=ON
cmake --build build/non_hardware-tsan --parallel
ctest --test-dir build/non_hardware-tsan --output-on-failure
```
