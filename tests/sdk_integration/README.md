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
The existing [portable suite](../non_hardware/README.md) and CI still work
without SDK headers; their tooling cases test stub generation with synthetic
declarations. Native cases are a separate optional CTest executable.

## Scope

Four registered cases cover:

- Real discovery/factory construction, mapped handles, rate/bandwidth
  configuration, invalid setup and inactive write rejection, CS16/CF32 RX and
  TX samples/timestamps, reverse-order completion, repeated stop/close/SDK
  shutdown. The two formats run through the same immediate-start path.
- Invalid input before SDK initialization and twelve scripted constructor
  failure points, checking that successful SDK acquisition is released once.
- Invalid clock/time constructor names reject before any SDK call; parser
  choices are also checked against the actual source metadata.
- A queue-full accepted prefix, active-close rejection, actual device
  destruction with a retained send, and subsequent callback delivery using
  the retained block/context. Cleanup guards deliver held contexts if an
  assertion fails as well.

Scripts use model/handle metadata and controlled sample values for host-path
testing; they do not advertise a physical card's capabilities. SDK transport
behavior, physical RF/PPS timing, callback quiescence/missing callbacks, and
multiple simultaneous device ownership remain outside this test. See the
[SDK lifetime evidence](../../docs/SDK_LIFETIME.md). Existing focused component
tests retain the detailed failure and concurrency matrices.
