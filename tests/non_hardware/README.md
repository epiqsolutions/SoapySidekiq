# Non-hardware tests

Requires CMake 3.21+ and a C++17 compiler. From the repository root, use the
SDK-free `non-hardware`, `non-hardware-asan`, `non-hardware-tsan`, or
`non-hardware-coverage` configure/build/test presets described in the main
[README](../../README.md#presets). For example:

```sh
cmake --preset non-hardware
cmake --build --preset non-hardware --parallel 2
ctest --preset non-hardware
tests/non_hardware/run_suite.sh build/non-hardware
```

The standalone configure commands below remain available. Coverage adds
`--coverage` only to the portable targets. Inspect its `.gcda`/`.gcno` files
with matching GCC `gcov` or Clang `llvm-cov gcov` tools; host coverage does not
measure the SDK-dependent device implementation.

On Linux, the tooling tests configure and link tiny SDK archive fixtures to exercise SDK
selection, version/error handling, quoted compile flags, and static link order.
Install/uninstall cases use temporary prefixes and DESTDIR staging, and check
that unrelated files survive. The simulated ARM SDK metadata case does not
cross-compile an ARM binary. The finder runs `sidekiq-config` as a host tool even
when the toolchain redirects program searches into a target root. The selected
test compiler is used by the fixtures.

These tests do not open a Sidekiq card or link to the Sidekiq SDK. They live
alongside the hardware validation programs under `tests/`.

When Python 3 is available, CTest also runs `tooling.all`. Its installer cases
configure a tiny project with real CMake, using command shims to prevent driver
builds, installation, or privileged commands. They cover positional and
environment prefixes, typed variables, spaces, precedence, and stale caches.
Synthetic tone tests use NumPy to check the production full-duplex detector,
including rejection of a wrong-tone quantization spur. Those cases explicitly
skip when NumPy is unavailable. The Python cases are not sanitizer-instrumented;
the sanitizer configuration instruments the C++ executables.
If interpreter discovery fails, configure with
`-DPython3_EXECUTABLE=/path/to/python3`; CMake reports when tooling tests are omitted.

Run tooling cases directly with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/non_hardware/tooling/run_tests.py
```

Configure and build:

```sh
cmake -S tests/non_hardware -B build/non_hardware
cmake --build build/non_hardware --parallel
```

To watch a run, start `tail -F out.txt` in another terminal, then run:

```sh
tests/run_logged.sh non_hardware_normal tests/non_hardware/run_suite.sh
```

The live output is `out.txt`, which points to `logs/current.log`; the runner
also keeps a per-run log in `logs/`. The suite writes its CTest output to
`build/non_hardware/suite.log`. The final line reports the number of
individual cases, rather than only the number of aggregate CTest executables.

The TX write regression executable compiles production TX methods against a
minimal device and scripted SDK boundary. CMake regenerates the extracted
bodies when production sources change and fails configuration if their
boundaries cannot be found. Cases compare every sample across multi-block
CF32/CS16 writes and cover clipping, shared deadlines, queue-full and busy-block
back-pressure, accepted prefixes and suffix retries, allocation/conversion
rollback, synchronous and deferred/out-of-order completion, cancellation, and
status polling. These fixtures do not construct a real SDK device or establish
physical RF behavior. The ASan/UBSan configuration also enables
`float-cast-overflow` and stops on sanitizer errors.

PPS cases cover both injected completed results and the actual production
activation/worker path. A controlled fake SDK call waits for an explicit test
edge: writes time out while pending, wake on success/failure publication, and
restart through an immediate activation without inheriting the old gate.
Deactivation cancels a waiting writer, retains the configured token/pool, and
joins startup before calling the hardware stop for either stop mode. Active
close is rejected without freeing the pool. The fake edge is always released
during test cleanup, including assertion failures.

These tests deliberately observe that deactivation remains blocked until the
pending SDK start returns. They do not establish cancellation of that SDK call,
real PPS timing, RX/TX start synchronization on a card, or full device teardown
while a real PPS edge is absent. The separate shutdown fixture checks device
cleanup ordering with an injected worker, rather than this SDK startup call.

The TX fixture also extracts the production native-format method to check
separate RX/TX host scales. The SDK unpacked-sample table specifies X2/X4/X40
TX as upper-14-bit samples in a 16-bit word: their host scale is 32767, not
8191. Tests assert explicit native scales and signed half-amplitude values
(16384 for upper-14-bit/16-bit, 1024 for right-justified 12-bit) across three
packets. They do not execute the constructor's SDK product/width queries or
measure RF output on hardware.

The gain fixture runs against both modern and SoapySDR 0.4-style range types.
The legacy type has only minimum/maximum bounds and no step constructor or
accessor. Both variants exercise the same production gain methods, channel
validation, conversions, SDK failures, and named controls. Modern range metadata
still reports RX profile increments and the TX quarter-dB increment; older
SoapySDR APIs can report only the bounds. This fixture checks API shape, not a
complete build/link against either real SoapySDR library.

Timestamp cases run both through the compiler-selected implementation and in
`timestamp_portable_tests`, which forces the portable integer fallback even
on hosts with `__int128`. Both cover truncation boundaries and signed output
overflow. On compilers with `__int128`, 10,000 deterministic input pairs also
check each implementation against a wide integer oracle.

RX queue tests retain raw RF/system timestamp phase across partial reads and
run through both native and forced portable arithmetic. A rational integer
oracle checks 1,000 blocks with eight partial reads each. The RX lifecycle
fixture compiles the production close/deactivate methods and RX activation
branch with the real RxStreamSession, an injected receive failure, and fake
SDK start/stop calls. It covers close/reuse, stop failure with retry, and
already-stopped hardware. Configuration cases reject rate/bandwidth writes in
either direction while RX retains ownership, including worker and stop failures.
A concurrent session test holds configuration open and verifies activation waits
before reading its snapshot and starting hardware. Stream setup, actual SDK I/O, and destructor cleanup
are outside that fixture's coverage.

The session component cases additionally cover concurrent activation during
worker teardown, retained ownership after stop errors or exceptions, and
fractional timestamps through both RF and system modes. Their backend receive
calls have a bounded wait, matching the production receive timeout even when
a hardware stop fails.

RX lifecycle regressions also check mixed PPS/immediate start and stop modes,
immediate close/destructor cleanup after PPS activation, and retrying a failed
PPS stop with an immediate stop. Startup rollback explicitly requests an
immediate stop; worker-creation failure is not injected by these tests.

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests/non_hardware -B build/non_hardware-asan \
    -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel
tests/run_logged.sh non_hardware_asan \
    tests/non_hardware/run_suite.sh build/non_hardware-asan
```

Stream diagnostic regressions install throwing log sinks while actual production
methods encounter TX preparation failure after an accepted prefix, activation
success/error, SDK status-read failure, failed-stop retry, and RX activation
validation failure followed by retry. They verify backend results/admission and
reservation/context cleanup survive failed diagnostics.

TX completion contexts retain shared bookkeeping rather than a device pointer.
Late-completion cases destroy the fixture device before invoking the production
callback and replace the stream state before completing an older reservation.
The production completion storage and locking are used with an instrumented
condition variable to preserve the existing wait-boundary tests. These cases
verify bookkeeping lifetime and stream isolation. The block-ownership cases
below extend them; callback delivery/quiescence after SDK shutdown is unverified.

TX blocks use a preallocated pool of shared owners with an injected SDK
allocator/deleter. Accepted contexts retain their own block through completion.
Allocation-failure cases run the production pool-initialization transaction at
each failed prefix and verify rollback preserves the published pool, bookkeeping,
and write index. Production close tests release unused storage while a pending
block retains its payload across close/replacement and completes afterward.
The device-destruction completion case also checks block lifetime. This does not
prove that SDK shutdown delivers every outstanding callback.

TX shutdown cases execute the production destructor, pool initialization, and
shared stop/drain helpers with tracked SDK calls and owned stream tokens. They
cover cleanup order, repeated shutdown, failed-stop retry, already-stopped
hardware, throwing diagnostics, idle destruction, blocks retained past SDK exit,
and joining a pending PPS worker before stop. A production activation case
checks that a post-start diagnostic failure preserves TX hardware ownership.
The pending worker is released by the fixture; no bounded SDK PPS cancellation
or guarantee of callback delivery is established.

Constructor-option tests bound `tx_buffer_count` to 1–30000 and reject malformed
values. Production writer cases use one- and two-block pools to verify ring
wrap, synchronous reuse, delayed-completion back-pressure, accepted prefixes, and
suffix retry. Default pool tuning still requires hardware throughput measurements.

Stream-method fixtures read the shared, RX, and TX implementation units. Complete
methods are extracted by signature and their unindented closing brace, preserving
the real bodies without depending on which implementation file contains the next
method. Multi-method fixtures compose their required bodies explicitly. The SDK
module builds and links all three units; these fixtures still use injected SDK
operations and do not replace a real module build.

The RX read boundary regression uses the real session/queue with a first-sample
timestamp just below int64 nanosecond overflow. It consumes one sample, then
checks that the next partial timestamp produces a stream error with cleared
metadata and untouched output, even when diagnostics throw. CS16 and CF32 both
recover after deactivation and reactivation with a representable epoch.
