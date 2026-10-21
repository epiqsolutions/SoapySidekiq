# Contributing

Keep each PR focused on one behavior change or one mechanical cleanup. Add a
regression test for a defect and keep functional changes separate from formatting,
renaming, or source splitting. Start new work from current master after its
predecessor merges; avoid long stacks.

## C++17 conventions

Use the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
as design guidance and the reviewed portable code under `include/SoapySidekiq`
as the local naming and ownership reference:

- Use four spaces, Allman braces, unindented namespaces, and a 100-column target.
  Keep include order explicit; the formatter does not sort includes.
- Use `PascalCase` types, `camelCase` functions, and descriptive local names.
  Follow the surrounding member convention when editing existing classes; do
  not rename SDK/SoapySDR overrides or unrelated code as a side effect.
- Prefer RAII and standard containers. Make resource ownership and callback
  lifetime explicit. Use scoped locks and document shutdown/wakeup ordering.
- Validate at the API boundary, preserve state on failure, and check SDK status.
  Use explicit checked numeric conversions; include operation/card/channel and
  backend status in new errors. Preserve established streaming status contracts.
- Add file-level Doxygen headers to new production C++ files and document reusable
  APIs, ownership, concurrency, and hardware constraints. Explain reasons rather
  than restating each statement.

These are conventions for focused new work. The integrated production sources
have completed the formatting pass, and clean GCC/Clang builds have a warning-free
baseline. Preserve that baseline when editing files; keep mechanical changes
separate from behavior changes.

The [channel validation audit](docs/ChannelValidationAudit.md) maps the concrete
device's implemented channel-taking APIs to validation helpers and regression
fixtures. Update that inventory when adding an overload or changing channel
selection; inherited defaults and pending contributions have separate limits.

## Device implementation boundary

Applications use the SoapySDR device and opaque stream API. The root
`SoapySidekiq.hpp` declares the module's concrete implementation; it is not an
installed application header. Keep stream tokens, callback contexts, workers,
and completion helpers private. Portable reusable contracts belong under
`include/SoapySidekiq` and are tested independently of device construction.

Forward-declare owned implementation types where their definitions are only
needed by out-of-line methods. Include those definitions directly in the source
files that use them. The device constructor and destructor stay out of line so
its `unique_ptr` owners can destroy forward-declared RX types with complete
definitions. The SDK types stored by value still require the SDK header.

## Reproducible tool versions

The initial Linux CI baseline is Ubuntu 22.04, GCC **11**, Clang **14**,
clang-format **14**, and clang-tidy **14**, with C++17. Local validation used
GCC 11.4.0, LLVM 14.0.0, CMake 3.22.1, and Python 3.10. CI records installed
versions on each run; distribution patch versions may change. Use versioned
LLVM commands so a default system upgrade does not silently change formatting.

The module needs SoapySDR development files and Sidekiq SDK **4.26+**. Public
CI builds portable production components and tests without that proprietary SDK;
it does not establish that the module builds/links or that a card works. The
module and standalone tests both require CMake 3.21+. The build respects the
caller's compiler/toolchain selection; see the [presets](README.md#presets).

On Ubuntu 22.04, install the portable baseline dependencies with:

```sh
sudo apt-get install g++-11 clang-14 clang-format-14 clang-tidy-14 python3-numpy
```

## Formatting focused changes

[`.clang-format`](.clang-format) defines the layout using
[LLVM 14's options](https://releases.llvm.org/14.0.0/tools/clang/docs/ClangFormatStyleOptions.html).
Format only the file or ranges you are deliberately cleaning up:

```sh
clang-format-14 --style=file --lines=20:45 -i path/to/file.cpp
python3 tools/check_format.py --base origin/master
```

The checker uses the merge base with the requested revision and checks added or
modified `.cpp`/`.hpp` lines in the committed, staged, and unstaged diff. Stage
new files first so Git includes them. Deleted files and untouched layout debt
are excluded. Clang-format can adjust the enclosing construct around a changed
line; review the proposed diff. The checker prints that diff and never edits
files. Invalid configuration, missing tools, or an invalid base fail the check.

## Compiler warnings and static analysis

The module and portable tests share `cmake/ProjectWarnings.cmake`: GCC/Clang
receive `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`.
Sidekiq/SoapySDR include directories are marked `SYSTEM` on the module target.
Warnings remain diagnostic: there is no global `-Werror`. Investigate new
warnings rather than hiding them with broad suppressions.

[`.clang-tidy`](.clang-tidy) enables a small set of correctness checks, with
project-header filtering and system-header diagnostics disabled. It does not
enable naming, modernization, or automatic fix passes. Generate a compile
database and run the same portable-production analysis as CI:

```sh
cmake -S tests/non_hardware -B build/analysis \
  -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
run-clang-tidy-14 -p build/analysis -j 2 -quiet \
  -clang-tidy-binary clang-tidy-14 \
  '/(DeviceDiscovery|DeviceOptions|SensorReader|StreamConfiguration|RxSampleQueue|RxStreamSession)\.cpp$'
```

The explicit correctness checks fail CI when found; compiler warnings are still
reported without promoting all compiler warnings to errors.
For SDK-dependent files, configure a real module build with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run, for example,
`clang-tidy-14 -p build/module Settings.cpp`. Never treat analysis of fake test
fixtures as analysis of the entire hardware-owning device.

## SDK errors and stream results

Keep error translation at the API or backend boundary that owns its contract:

| Boundary | Failure policy |
| --- | --- |
| RF, settings, clock/time, metadata | Throw a descriptive exception for invalid requests or SDK failures. |
| Stream setup/close and unsigned MTU queries | Throw for invalid arguments or lifecycle failures; never encode a negative status in an unsigned result. |
| Status-returning stream calls | Return signed SoapySDR codes for routine SDK errors, timeouts, and back-pressure; preserve an accepted TX sample prefix. |
| Sensor reads | Translate typed backend errors to the documented legacy string fallback and diagnostics. |
| SDK callbacks/workers | Publish completion/error state before diagnostics and contain exceptions at the C/thread boundary. |

A single unconditional SDK-status-to-exception helper would break these distinct
contracts. Share validation and diagnostic containment where their semantics
match. Include operation, card, channel/handle, requested value, and SDK status
when available. Use `reportDiagnostic` only around synchronous diagnostics in a
status-returning path; backend operations and their errors must stay outside it.
A failing user log sink must not replace the operation's result or accepted prefix.

## Local artifacts and JSON source files

Keep build outputs under `build/` and captured logs/JSON results under `logs/`.
Those directories are ignored, while `logs/README.md` remains tracked. The root
`compile_commands.json` and `CMakeUserPresets.json` are machine-specific and
ignored; the shared `CMakePresets.json` is source-controlled.

JSON source fixtures and configuration files are visible to Git. Do not add a
blanket extension ignore for them; put generated results in the artifact
directories instead.

## API documentation

For a small test across the concrete driver and its SDK backend, see the
[optional native scripted-SDK integration test](tests/sdk_integration/README.md).
It compiles against installed SDK headers while linking scripted/fail-fast SDK
definitions, so its cases require no cards. Keep its cross-component scenarios
small; detailed behavior matrices belong in the focused portable fixtures.

The [software closeout record](docs/validation/2026-10-21-software-closeout.md)
pins the combined-tree builds, tests, analysis, and documentation checks. The
earlier [integration checkpoint](docs/validation/2026-10-21-integration.md) also
records install-layout exercises. Repeat affected checks after later changes;
neither record is hardware qualification or a final release sign-off.

Generate the production API and reusable-component reference from the repository
root with Doxygen 1.9.1 or later:

```sh
mkdir -p build/api-docs
doxygen docs/Doxyfile
```

CI runs the same SDK-free Doxygen check in the analysis job and saves its
warning log. Warnings fail that check, so document new public declarations and
reusable fields when adding them. It also checks private members, static helpers,
and anonymous-namespace declarations with a separate internal reference:

```sh
mkdir -p build/internal-docs
doxygen docs/Doxyfile.internal
```

Document owned/borrowed lifetimes, lock protection, units, and failure semantics
where they are declared. The internal reference and warning log are generated
under `build/internal-docs`; private declarations stay out of the public reference.

Open `build/api-docs/html/index.html`; diagnostics are written to
`build/api-docs/warnings.log`. The configuration treats undocumented public
members and invalid documentation commands as errors. The internal configuration
inherits that policy and adds private/file-local declarations. Generated files
stay under the ignored build directory; SDK/SoapySDR development headers are not required for this
documentation-only command. Update the declaration's contract when changing
behavior instead of copying the same prose into its implementation.

## Tests and CI

Run the [non-hardware suite](tests/non_hardware/README.md) before pushing:

```sh
cmake -S tests/non_hardware -B build/non_hardware \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-11 \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/non_hardware --parallel 2
tests/non_hardware/run_suite.sh build/non_hardware
cmake -S tests/non_hardware -B build/non_hardware-asan \
  -DCMAKE_BUILD_TYPE=Debug -DSOAPYSIDEKIQ_TEST_ASAN=ON \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/non_hardware-asan --parallel 2
tests/non_hardware/run_suite.sh build/non_hardware-asan
```

The root `non-hardware` and `non-hardware-asan` presets configure the same suite
without looking for the SDK. CI uses those presets. TSan and coverage have
separate presets too; a TSan build alone does not establish that its runtime
works on the host. SDK-selection/link-usage and staged-uninstall regressions
use small real-CMake fixtures, without requiring the proprietary SDK or root.

The workflow runs GCC 11, GCC 11 ASan/UBSan, Clang 18 TSan, Clang 14,
changed-line formatting, and portable static analysis on PRs and master pushes. It saves counted case
results and analysis output as artifacts. NumPy and clang-format are installed
so the Python tooling regressions run as well. The full SDK build and
[hardware validation](tests/hardware/README.md) remain separate checks; rerun
the relevant hardware cases when production behavior changes.

## Legacy implementation proposals

The [legacy prototype evaluation](docs/LEGACY_PROTOTYPES.md) compares the old
channel mapper and asynchronous TX pool/writer with the production contracts.
Consult its ownership requirements before adapting those prototypes, especially
for callback-visible state, partial writes, and stream shutdown.

## ThreadSanitizer host setup

The TSan CI job runs the portable suite with Clang 18 on an Ubuntu 24.04 VM.
It installs/selects GCC 11's C++ development headers explicitly to match the
validated baseline, independent of newer GCC packages on the hosted image.
Address randomization is disabled for the runner process and its children. It uses
`setarch`, not a host-wide sysctl change. `halt_on_error=1` fails the affected
executable on its first race report; the counted runner preserves failures
and saves `suite.log`/CTest output as usual. A startup failure fails the job
rather than skipping instrumented executables.

To use the same setup on a compatible Ubuntu x86-64 host with Clang 18 and
GCC 11 development packages installed:

```sh
cmake --preset non-hardware-tsan -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11 \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset non-hardware-tsan --parallel 2
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 \
  tests/non_hardware/run_suite.sh build/non-hardware-tsan
```

Use Clang 18's runtime for this job: older installed runtimes can miss timed
wait/lock interceptors and report false mutex errors. LLVM 18 includes the
[`pthread_mutex_clocklock` interceptor](https://github.com/llvm/llvm-project/blob/llvmorg-18.1.8/compiler-rt/lib/tsan/rtl/tsan_interceptors_posix.cpp).
The GCC project also records the earlier
[`pthread_cond_clockwait` false-positive issue](https://gcc.gnu.org/pipermail/gcc-patches/2021-May/570244.html).
Do not suppress those functions or alter driver synchronization to bypass an
incomplete runtime. The regular compiler baseline remains GCC 11 and Clang 14;
Clang 18 is an additional sanitizer configuration.

Some containers prohibit the personality change, and some runtime/host mappings
are incompatible with TSan. Record that environment failure and use a compatible
host; do not count compilation or a skipped binary as a passing run. This job
covers the portable tests, not the proprietary SDK, hardware callbacks, or the
entire concrete driver.
