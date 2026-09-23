# Non-hardware tests

These tests are separate from the hardware validation programs in `tests/`.
They do not open a Sidekiq card or link to the Sidekiq SDK. Output from the
repository runner is written to the ignored `out.txt` file.

Configure and build:

```sh
cmake -S non_hardware_tests -B build/non_hardware
cmake --build build/non_hardware --parallel
```

To watch a run, start `tail -F out.txt` in another terminal, then run:

```sh
non_hardware_tests/run_suite.sh
```

The final line reports the number of individual cases, rather than only the
number of aggregate CTest executables.

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S non_hardware_tests -B build/non_hardware-asan \
    -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel
ctest --test-dir build/non_hardware-asan --output-on-failure
```
