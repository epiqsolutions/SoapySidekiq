# Integration validation checkpoint, 2026-10-21

Tested source: master `be1364ec9294c9a75bd1eda1e40468b25ed82e11`, after
the private ownership documentation and lock-contract corrections in #126.
The test/compiler cleanup in #127 is included. Later PRs, including #128's
RF/session tests and #129's private TX naming changes, are outside this pin.

This is an interim software/build checkpoint. It is not a release candidate or
a new hardware run. Draft API/card contributions #80–#86 are not included;
repeat affected checks after their integration and on the final release tree.

## Results

| Check | Result and scope |
| --- | --- |
| Non-hardware suite, GCC 11 | 477 individual cases pass. |
| Non-hardware suite, GCC ASan/UBSan | 477 individual cases pass. |
| Native module, GCC 11 and Clang 14 | Clean builds, links, isolated installs, and driver-load checks pass with SDK 4.26 and SoapySDR 0.8.1. No compiler warnings in either clean build log. |
| Legacy SoapySDR | Clean module build, install, and driver-load checks pass against the real SoapySDR 0.4.0 tag, ABI `0.4-2`, with SDK 4.26. The actual installed module path appears in utility information output. No card was opened. |
| AArch64 | Clean cross-build, link, and install pass; the installed module is ELF AArch64. Target execution and hardware behavior are untested here. |
| Formatting and file headers | All 36 tracked production C++ source/header files match clang-format 14 and contain their file-level Doxygen block. |
| Documentation | Public and internal Doxygen configurations report zero warnings. Tracked Markdown relative file links resolve; this does not check external URLs or section anchors. |
| Static analysis | The configured clang-analyzer/bugprone checks process all 17 root module implementation units using the native Clang compile database, with no reported findings. This is the conservative configured check set, not an exhaustive analysis claim; clang-tidy's generated/filtered diagnostic counts are separate from compiler warning counts. |
| Install layout | A custom prefix containing spaces can be relocated and its actual module loaded. `/usr` and `/usr/local` installs stage beneath local `DESTDIR` roots and preserve unrelated sentinel files. No system-prefix installation was performed. |
| Source/artifact inventory | All 17 root production `.cpp` files occur in the native compile database. No tracked build artifacts, disabled `#if 0` blocks, debug macros, or commented-out statements were found in the production scan. The scan does not establish that every retained member or branch is necessary. |

## Reproducing the checks

Run from the repository root at the pinned source. Compiler/dependency paths
must refer to the stated versions; the configured build directories below are
local to the validation node. See [CONTRIBUTING](../../CONTRIBUTING.md) for
fresh setup and [the ARM guide](../AARCH64_BUILD.md) for cross-toolchain setup.

```sh
cmake --build --preset non-hardware --parallel 2
tests/non_hardware/run_suite.sh build/non-hardware
cmake --build --preset non-hardware-asan --parallel 2
tests/non_hardware/run_suite.sh build/non-hardware-asan

cmake --build build/header-gcc --clean-first --parallel 2
cmake --install build/header-gcc
env SOAPY_SDR_ROOT="$PWD/build/header-gcc-install" SoapySDRUtil --check=sidekiq
cmake --build build/header-clang --clean-first --parallel 2
cmake --install build/header-clang
env SOAPY_SDR_ROOT="$PWD/build/header-clang-install" SoapySDRUtil --check=sidekiq

run-clang-tidy-14 -p build/header-clang -j 2 -quiet \
  -clang-tidy-binary clang-tidy-14 '/home/dhelm/SoapySidekiq/[^/]+\.cpp$'
mkdir -p build/api-docs build/internal-docs
doxygen docs/Doxyfile
doxygen docs/Doxyfile.internal

env SIDEKIQ_CONFIG_BUILD_CONFIG=aarch64 \
  cmake --build build/aarch64-validation/rf-module --clean-first --parallel 2
cmake --install build/aarch64-validation/rf-module
readelf -h build/aarch64-validation/install/lib/SoapySDR/modules0.8-3/libSidekiqSupport.so
```

The install exercises used `cmake --install build/header-gcc --prefix ...`
with an isolated prefix containing spaces, then renamed that prefix before
running `SoapySDRUtil --info` and `--check=sidekiq`. The information output
identified the relocated module. Separate local `DESTDIR` values contained
the `/usr` and `/usr/local` staging tests. The normal isolated install manifest
was restored afterward. The legacy build used the existing real-0.4.0 setup
under `build/soapy-0.4.0-validation` and its own utility/library search path.

Node-local logs remain under ignored `build/`:
`integrated-audit-tests.log`, `integrated-audit-native-gcc.log`,
`integrated-audit-native-clang.log`, `integrated-audit-native-tidy.log`,
`integrated-audit-arm.log`, `integrated-audit-soapy04.log`, and
`integrated-audit-installs.log`. The ARM log includes a corrected artifact-path
lookup after an initial inspection used the wrong filename; the build/install
and subsequent inspection of the actual installed file passed. These local
paths are not remotely attached raw evidence.

## Remaining validation

TSan still needs a compatible host; this node previously failed to start its
runtime with an unexpected memory mapping. No new TSan pass is claimed.
Actual ARM execution, shared-card SDK ownership, callback delivery/quiescence,
absent-edge PPS cancellation, and TX buffering throughput measurements remain
open. Native module loading does not open a card or test RX/TX samples.

The [post-update hardware procedure](../../tests/hardware/POST_UPDATE_VALIDATION.md)
describes the new run and evidence required after integration. Historical
hardware results in the [release draft](../RELEASE_NOTES.md) apply only to
their earlier candidate. Preserve those boundaries when preparing the release.
