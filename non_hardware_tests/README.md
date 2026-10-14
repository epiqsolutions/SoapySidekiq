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

The TX write regression executable compiles the `writeStream` method body
directly from `Streaming.cpp` against a minimal device and synchronous SDK
fixture. CMake regenerates that body when the production source changes and
fails configuration if the method boundaries cannot be found. These tests
compare every sample in three CF32 and CS16 packets. They exercise successful
input advancement, not hardware setup, allocation rollback, asynchronous
callbacks, queue-full handling, or conversion clipping policy. CF32 coverage
includes negative normalized components, zero, and both signed full-scale
endpoints. The ASan/UBSan configuration also enables `float-cast-overflow` and
stops on sanitizer errors.

Timestamp cases run both through the compiler-selected implementation and in
`timestamp_portable_tests`, which forces the portable integer fallback even
on hosts with `__int128`. Both cover truncation boundaries and signed output
overflow. On compilers with `__int128`, 10,000 deterministic input pairs also
check each implementation against a wide integer oracle.

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S non_hardware_tests -B build/non_hardware-asan \
    -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel
ctest --test-dir build/non_hardware-asan --output-on-failure
```
