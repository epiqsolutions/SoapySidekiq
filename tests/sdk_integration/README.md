# Concrete-driver integration with a scripted SDK

This optional native Linux test compiles the actual concrete driver, registry
adapter, SDK backend, and stream owners. It requires an installed Sidekiq SDK
4.26 for headers/build metadata, plus SoapySDR development files. It links SoapySDR and threads, with no
Sidekiq SDK library and no external plugin loading or hardware access.

The SDK's inline memory allocation helpers still compile from its headers.
Scripted functions implement the calls used by the test. Every other referenced
SDK symbol receives an aborting definition generated from the local SDK
declaration. Missing declarations fail generation; unexpected calls name the
operation and abort. Generated SDK declarations stay in the build directory.

## Build and run

From the repository root:

```sh
cmake -S . -B build/scripted-sdk \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DSOAPYSIDEKIQ_BUILD_SDK_INTEGRATION_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/scripted-sdk-install"
cmake --build build/scripted-sdk --target scripted_driver_tests --parallel 2
ctest --test-dir build/scripted-sdk -R '^scripted-driver.all$' --verbose
```

Adjust the Python path if needed. The normal SDK/SoapySDR discovery options,
including `SIDEKIQ_SDK_DIR` and `SoapySDR_DIR`, apply. The native module
configuration must remain enabled; the test target uses its production source
list. No test target is installed, and the ordinary module has no fake symbols.

Use another build directory for Clang or sanitizers. For ASan/UBSan add
`-DSOAPYSIDEKIQ_SDK_INTEGRATION_ASAN=ON` at configure time. The sanitizer test
uses a non-PIE executable, matching the portable suite's Linux ASan setup.
For ThreadSanitizer use a separate Clang 18 build with
`-DSOAPYSIDEKIQ_SDK_INTEGRATION_TSAN=ON`. ASan/UBSan and TSan cannot be enabled
together. On Ubuntu x86-64 with Clang 18 and GCC 11 development headers:

```sh
cmake -S . -B build/scripted-sdk-tsan \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11 \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DSOAPYSIDEKIQ_BUILD_SDK_INTEGRATION_TESTS=ON \
  -DSOAPYSIDEKIQ_SDK_INTEGRATION_TSAN=ON
cmake --build build/scripted-sdk-tsan --target scripted_driver_tests --parallel 2
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/scripted-sdk-tsan -R '^scripted-driver.all$' --verbose
```

The sanitizer options apply to the test objects and executable, including
scripted SDK functions and generated fail-fast stubs. They do not instrument
the normal module or the linked SoapySDR library. Missing timed-lock/wait
interceptors in older runtimes can produce false reports; use a matched newer
compiler/runtime rather than suppressing synchronization functions. This setup
changes ASLR only for the test process and its children.

The existing [portable suite](../non_hardware/README.md) and CI still work
without SDK headers; their tooling cases test stub generation with synthetic
declarations. Native cases are a separate optional CTest executable.

## Scope

The registered cases cover:

- Real discovery/factory construction, mapped handles, rate/bandwidth
  configuration, invalid setup and inactive write rejection, CS16/CF32 RX and
  TX samples/timestamps, reverse-order completion, repeated stop/close/SDK
  shutdown. The two formats run through the same immediate-start path.
- Invalid input before SDK initialization and twelve scripted constructor
  failure points, checking that successful SDK acquisition is released once.
- Invalid clock/time constructor names reject before any SDK call; parser
  choices are also checked against the actual source metadata.
- Optional topology/clock/disabled-PPS construction applies configuration before
  publishing callbacks. Unsupported topology, topology/clock SDK errors, and
  an initial PPS timestamp-read error release SDK ownership exactly once and
  permit a clean construction retry. The PPS error occurs before its observation
  wait; this case does not touch the kernel fallback or supply a physical edge.
- A queue-full accepted prefix, active-close rejection, actual device
  destruction with a retained send, and subsequent callback delivery using
  the retained block/context. Cleanup guards deliver held contexts if an
  assertion fails as well.

- Sixteen construction/TX/teardown cycles deliver real completion callbacks
  from another thread while the actual device is destroyed. Delivery may finish
  before, during, or after teardown; each cycle checks both accepted callbacks,
  one TX stop, one SDK exit, and no pending scripted sends.

- A 28-method scalar-channel matrix makes 672 rejected requests across invalid
  directions, count-boundary/maximum indices, and both/RX-only/TX-only/absent
  channel configurations. Each operation must fail before any scripted SDK call
  and leave the native-format output unchanged. This is one registered case;
  inherited defaults and excluded draft API additions are outside its inventory.

- Two concrete PPS lifecycle cases hold the real TX startup worker inside a
  scripted SDK call. Immediate RX remains usable, zero-time writes time out,
  deactivation cancels a waiting writer before startup returns, and PPS stop
  follows the worker's success/error return. A destructor case retains SDK state
  until the held worker finishes, then checks stop/unregister/exit order.
  Cleanup guards release the script before joining futures on assertion failure.

- Three concrete stream-validation cases reject null/inaccessible/closed tokens,
  wrong directions, unsupported activation/deactivation/write flags, finite
  bursts, invalid counts/buffers, and negative timeouts. They verify no SDK
  access, no pending sends, and reuse of valid tokens after rejected controls.
  SDK block-size queries and transmit attempts are explicitly recorded.

- Two failed-stop/retry cases retain RX configuration ownership across failed
  deactivation and close retries, reject premature restart/RF writes, and then
  restart RX with fresh coupled-clock timestamps. TX host submissions preserve
  their payloads across that RX sequence. A failed TX stop retains accepted
  contexts and the token while closing write admission; callbacks and a later
  successful retry release the owners without duplicate successful stops.
  This checks host control/payload behavior, not physical RF continuity.

The scripted object target alone enables two observation points in
`TxStreaming.cpp`: after writer admission on entering the PPS-wait path, and
after draining callers on entering the PPS-worker join path. Mutex-protected
counters and a condition variable confirm those phases before tests launch
shutdown or release SDK startup. A baseline count distinguishes the async
writer from an earlier zero-time probe. Future timeouts are secondary checks,
not evidence that an async task started. Hooks record entry without modifying
device state or waiting for a test release; the normal module has no hooks or
additional symbols, and the concrete device layout is unchanged.

Scripts use model/handle metadata and controlled sample values for host-path
testing; they do not advertise a physical card's capabilities. SDK transport
behavior, physical RF/PPS timing, callback quiescence/missing callbacks, and
multiple simultaneous device ownership remain outside this test. See the
[SDK lifetime evidence](../../docs/SDK_LIFETIME.md). Existing focused component
tests retain the detailed failure and concurrency matrices.
