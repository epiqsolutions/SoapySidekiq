# SoapySidekiq

SoapySDR support module for Epiq Solutions Sidekiq SDR devices.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the C++17 coding baseline, formatter,
static analysis, compiler warnings, and CI/non-hardware test instructions.

## Minimum dependencies

- CMake 3.21 or newer (including standalone tests and preset commands)
- A C++17-capable compiler
- SoapySDR development files, version 0.4.0 or newer
- Sidekiq SDK version 4.26.0 or newer

## Sidekiq SDK discovery

The build locates the Sidekiq SDK in this order:

1. `-DSIDEKIQ_SDK_DIR=/path/to/sdk`
2. `SIDEKIQ_SDK_DIR` from the environment
3. `$HOME/sidekiq_sdk_current`

The selected SDK must provide `bin/sidekiq-config` and be version 4.26.0 or
newer. The installer does not bundle an SDK.

## Build

Configure and build with CMake:

```bash
cmake -S . -B build
cmake --build build
```

Module installs respect `CMAKE_INSTALL_PREFIX`, `cmake --install --prefix`, and
`DESTDIR` staging, including with SoapySDR 0.4.0/0.4.1's older module helper.
An explicit module prefix does not install beside the SoapySDR dependency.

The build respects `CXX`, `-DCMAKE_CXX_COMPILER=...`, and CMake toolchain files.
The module requests C++17 without compiler extensions. For example:

```bash
cmake -S . -B build/clang -DCMAKE_CXX_COMPILER=clang++-14
cmake --build build/clang --parallel 2
```

The `Sidekiq::sidekiq` imported target carries the SDK include paths, compiler
options, and ordered static-link flags. Changing `SIDEKIQ_SDK_DIR` in an existing
build directory reselects `sidekiq-config` without clearing its cache manually.
For cross-compilation, supply your toolchain file and a compatible SDK build
configuration through the SDK's `SIDEKIQ_CONFIG_BUILD_CONFIG` environment
variable. The ARM compiler/sysroot and SDK binaries must match; a host build
does not validate that combination. See the tested [AArch64 cross-build
workflow](docs/AARCH64_BUILD.md) and optional GNU toolchain file.

### Presets

List presets with `cmake --list-presets`. They respect the chosen generator and
compiler and use separate build directories:

| Preset | Purpose | SDK required |
| --- | --- | --- |
| `dev` | Debug module; install prefix `build/dev-install` | Yes |
| `non-hardware` | Portable regression suite | No |
| `non-hardware-asan` | Portable suite with ASan/UBSan | No |
| `non-hardware-tsan` | Portable suite with TSan on a compatible host | No |
| `non-hardware-coverage` | Portable suite with GCC/Clang coverage | No |

```bash
cmake --preset dev
cmake --build --preset dev --parallel 2
cmake --install build/dev
SOAPY_SDR_ROOT="$PWD/build/dev-install" SoapySDRUtil --check=sidekiq

cmake --preset non-hardware
cmake --build --preset non-hardware --parallel 2
ctest --preset non-hardware
tests/non_hardware/run_suite.sh build/non-hardware
```

Presets export `compile_commands.json` in their build directories. Keep local
overrides in ignored `CMakeUserPresets.json`. The schema uses
[CMake 3.21 presets](https://cmake.org/cmake/help/v3.21/manual/cmake-presets.7.html).
These instrumentation presets apply to portable tests. ASan/UBSan, TSan, and
coverage use separate presets/directories; do not enable them together. The
optional [concrete-driver integration target](tests/sdk_integration/README.md)
has its own ASan/UBSan option and requires SDK headers, with no hardware access.

Without presets, `SOAPYSIDEKIQ_BUILD_MODULE` defaults to `ON`, and
`SOAPYSIDEKIQ_BUILD_TESTS` defaults to `OFF`. Set the former to `OFF` and the
latter to `ON` for SDK-free root builds. The standalone
`cmake -S tests/non_hardware -B ...` interface remains supported too.

### Installation and removal

Install the module with:

```bash
sudo cmake --install build
sudo ldconfig
```

An explicit `-DCMAKE_INSTALL_PREFIX=...` wins; otherwise the default follows
SoapySDR's prefix when SoapySDR is outside `/usr`. Install over an existing
prefix with `cmake --install` again. Use a separate build directory when changing
compilers or toolchains.

The module build provides an `uninstall` target that removes files in its latest
`install_manifest.txt`, leaves directories and unrelated files intact, and
accepts the same `DESTDIR` used for a staged installation:

```bash
cmake --build build/dev --target uninstall
```

Use elevated permissions for a system-prefix install/uninstall. Installing with
`--prefix` updates the manifest for that installation. Keep the manifest until
removal; the target fails if no install has been recorded.

The `install.sh` helper builds in `build/install-driver` without deleting other
build directories. Its optional argument is a CMake variable such as
`SIDEKIQ_SDK_DIR=/path/to/sdk`. Set `SOAPYSIDEKIQ_INSTALL_PREFIX` to install
without `sudo` into an isolated location:

```bash
SOAPYSIDEKIQ_INSTALL_PREFIX="$PWD/build/local-install" \
  ./install.sh SIDEKIQ_SDK_DIR=/path/to/sdk
SOAPY_SDR_ROOT="$PWD/build/local-install" SoapySDRUtil --info
```

An explicit prefix also works as `./install.sh CMAKE_INSTALL_PREFIX=/path/to/install`
(or `CMAKE_INSTALL_PREFIX:PATH=/path/to/install`). This argument overrides the
environment prefix. Both forms install without `sudo`; the chosen directory
must be writable by the caller. Without either prefix, the helper clears a
previous isolated prefix, installs into CMake's default system prefix with
`sudo`, and runs `ldconfig`. Set `SOAPYSIDEKIQ_BUILD_DIR` to use a different
build directory.

`remake.sh` forwards to `install.sh`; it no longer requires Bear, cleans a build
tree, or copies a compile database into the repository root. Use the database
generated in the selected build directory directly.

## Constructor topology selection

The optional `topology` device argument selects an SDK topology ID. An explicit
request on a card without topology support raises an argument error instead of
silently retaining a different configuration. Supported cards pass the ID to
the SDK and report application failures with card/topology/status context.
Omit the argument to retain the card's default topology behavior.

Optional `clock_source` and `time_source` names are checked before SDK
initialization, using the case-insensitive choices documented below. Unknown,
empty, or whitespace-padded names fail without opening the card. Supported
names still require the card/SDK to accept the requested configuration.

## Streaming limitations

The current streaming implementation supports:

- at most one active RX stream per device
- at most one active TX stream per device
- one requested channel per stream

Simultaneous RX and TX are supported. Multi-channel Soapy streams are not currently implemented.

There are no configurable per-stream arguments. `getStreamArgsInfo()` returns
an empty list. A nonempty per-stream argument map now raises an argument error
before SDK access, including the former unused `bufflen` option. Constructor
options belong in the device arguments, rather than the stream arguments.
Query `getStreamMTU()` after setup for the actual buffer sample count. RX uses
the SDK's balanced receive mode. Set the device constructor argument
`tx_block_size` to choose a validated TX payload size before stream setup.

TX accepts CS16 or normalized CF32 and requires each nonempty `writeStream`
request to contain a whole number of `getStreamMTU()` samples, with at most
`INT_MAX` samples per call. It does not stage partial hardware blocks. Invalid
counts or null nonempty buffers return `SOAPY_SDR_STREAM_ERROR`; a zero-length
write on an admitted TX stream returns zero without reading buffers. If
back-pressure accepts only a prefix, advance the input by that many complex
samples and retry the suffix. A hard SDK submission failure or packet
preparation failure returns `SOAPY_SDR_STREAM_ERROR` if no samples were
accepted; otherwise it returns the accepted prefix. The rejected packet's
reservation/context are released, while accepted packets retain completion
ownership.

TX write admission opens after activation and closes when deactivation begins.
Writes before activation or after deactivation return `SOAPY_SDR_STREAM_ERROR`,
including zero-length requests. A configured stream token alone does not open
write admission.

CF32 components clip to [-1, 1], infinities clip to the corresponding endpoint,
and NaN converts to zero before scaling to the card's TX full scale.

The constructor argument `tx_buffer_count` selects 1–30000 preallocated host TX
blocks; the compatibility default remains 30000. It is independent of
`tx_block_size`, which defaults to 16380 complex samples. For example,
`driver=sidekiq,card=0,tx_buffer_count=1024` uses 1024 blocks. Smaller pools reduce
host memory and the number of outstanding sends, and may reach back-pressure
sooner; writes retain their timeout and accepted-prefix contract.

Payload plus SDK header storage is `4 * (tx_block_size + 4) * tx_buffer_count`
bytes, excluding allocator, shared-owner, and status overhead. At the default
block size this is 64 MiB for 1024 blocks and about 1.83 GiB for 30000 blocks.
The limit bounds the count, rather than the total bytes; a larger block size
also increases memory use. A lower default awaits throughput/latency measurements
on supported cards.

`listSampleRates()` retains a compatibility list spaced at 250 kHz within the
SDK's reported bounds. It does not enumerate the exact profiles of cards with
discrete rate support; the SDK validates each requested rate.

Antenna APIs expose the currently configured RF port. Selecting that same
port is supported; switching to another port is not yet implemented. Cards
without RF-port reporting return no antenna names.

RX automatic DC-offset correction selects the DC calibration type and enables
the SDK's shared automatic calibration mode. Other selected calibration types
are preserved and can also run automatically in that mode. Disabling DC-offset
correction clears only its selection bit and preserves the shared mode.
The SDK can run selected calibrations during subsequent RX tuning operations.

`listSampleRates()` suggests documented equal-rate built-in profiles for NV100
and NVM2, filtered by the SDK's card and selected-channel bounds. A continuous
hardware range does not imply that every intermediate value is a supported
profile. Disparate RX/TX rates and custom profiles require their own configuration;
the suggestion list does not enumerate those combinations.

Automatic gain control is exposed for RX. TX uses manual attenuation and
reports `hasGainMode()` as false; requesting TX automatic mode raises an error.

`setHardwareTime` accepts nonnegative nanoseconds with an empty/`now` selector
for an immediate update or `1pps` for the next PPS. Conversion uses the system
clock frequency and truncates fractional ticks with checked integer arithmetic.
The SDK immediate update sets both RF and system counters to the same raw tick
value; the PPS update sets RF to zero and system to the requested tick value.
The counters can therefore represent different times when their rates differ.

PPS source selection rejects unknown names. If the SDK source API fails, the
existing kernel PPS endpoint is used when available. Selecting external or
internal PPS requires a timestamp transition across one one-second observation
interval; missing edges, SDK errors, and interrupted waits raise an error.
Source configuration can already have changed when pulse verification fails.
The kernel fallback can select external/internal GPS, but cannot disable PPS.

Clock selection accepts `internal_clock` or `external_clock` (case insensitive)
and rejects other names before accessing the SDK. Actual source support is
card dependent; SDK failures are returned to the caller. The SDK host reference
retains the `internal_clock` alias. Reference rates describe configuration,
not measured clock presence or lock; zero rates are rejected. External-rate
readback depends on SDK/card support.

Device setting keys are case insensitive. `iq_swap` and `counter` accept
`true`/`false` (case insensitive) or `1`/`0`; other values raise an error.
`sys_clock_freq`, `full_scale`, `overload`, and `cal_offset` are read only.
Unknown keys raise an error. Metadata reports the current cached settings.
Counter mode applies to all RX handles and, per SDK requirements, takes effect
when RX streaming next starts. Failed multi-handle changes attempt to restore
all affected handles; rollback failures are reported and require recovery.
Timestamp selection accepts `rf_timestamp` or `sys_timestamp`; configure it
before starting RX streaming.

Stream tokens belong to the device that created them and become invalid on
close. Unknown tokens return `SOAPY_SDR_STREAM_ERROR` from activation,
deactivation, I/O, and status calls. `closeStream` and `getStreamMTU` raise
an argument error for an unknown token; MTU failures never return an unsigned
version of a negative error code.

RX activation reads the current actual sample rate and, when selected, the
current system timestamp frequency. Fractional sample rates retain their exact
binary value through the worker and partial-read timestamp calculation.
Sample-rate and bandwidth setters in either direction reject changes while RX
owns hardware or a receive worker, since RX/TX clocks may be coupled. Successfully
deactivate RX before changing them; a failed stop or failed worker still blocks
these setters. Activation captures its timing snapshot under the same lock.
Clock source, PPS source, hardware-time updates, and `timetype` changes use the
same inactive-RX guard. Configure them before activating RX. Rejection occurs
before SDK reads/writes, kernel PPS fallback, or timestamp-selection cache changes.
A hardware-time update scheduled for PPS can still take effect after this setter
returns; configure and observe the intended PPS edge before capturing data. The
guard does not cancel scheduled SDK updates or a blocking PPS start.

RX read exceptions from allocation or timestamp arithmetic return
`SOAPY_SDR_STREAM_ERROR` with flags and output timestamp cleared. A failing
log sink cannot replace that error result. After an unrepresentable timestamp,
deactivate and reactivate with a valid timestamp epoch before continuing.

`getHardwareTime` uses system time for the empty selector or `sys_timestamp`.
Explicit `rx_rf_timestamp` and `tx_rf_timestamp` read the first mapped handle
with the SDK's current actual rate, preserving fractional Hz. Missing rates,
unsupported directions, unknown selectors, and nanosecond overflow raise errors.

TX `readStreamStatus` waits for a new underrun count or returns
`SOAPY_SDR_TIMEOUT`. An underrun reports stream channel mask `1`, flags `0`,
and no timestamp. SDK errors, failed PPS startup, and shutdown return
`SOAPY_SDR_STREAM_ERROR`. Shutdown wakes status waiters and drains admitted
status calls before allowing stream cleanup. Each successful TX start resets
the host underrun baseline to match the SDK.

Stream activation/deactivation accepts flags `0` for immediate operation, or
`SOAPY_SDR_HAS_TIME` with `timeNs=0` for the driver's existing next-PPS behavior.
Absolute timed starts/stops, additional flags, and finite-burst activation
(`numElems != 0`) return `SOAPY_SDR_NOT_SUPPORTED`. TX writes accept flags `0`;
timed writes, end-of-burst, and fragment flags are unsupported. As in the
SoapySDR contract, `timeNs` is ignored when `HAS_TIME` is absent.

`activateStream()` converts SDK, timing-readback, and session-start exceptions
into logged `SOAPY_SDR_STREAM_ERROR` results. Unsupported options still return
`SOAPY_SDR_NOT_SUPPORTED`. TX activation requires prior deactivation when a
stream is already active or still owns a PPS startup thread, including a failed
PPS start. This keeps repeated activation from replacing a joinable thread.

Status-returning stream calls contain exceptions from user-installed diagnostic
sinks. Logging failures do not turn a successful activation into an error,
replace an SDK failure code, or hide an already accepted TX sample prefix.
This diagnostic policy does not suppress backend failures.

SDK stop failures from `deactivateStream()` return `SOAPY_SDR_STREAM_ERROR` and
retain stream ownership so the caller can retry. TX stops keep new writes
blocked after failure. `closeStream()` raises an exception if RX hardware
cannot be stopped and retains the stream for recovery; active TX must be
deactivated successfully before it can be closed.

Device destruction closes TX write/status admission, drains admitted calls, and
joins TX PPS startup before stopping TX, releasing stream storage, and removing
callbacks. A pending SDK PPS start can still keep shutdown waiting for its edge;
this cleanup does not cancel that SDK call. Accepted contexts retain their
individual blocks and completion state; callbacks that never arrive remain an
unresolved SDK shutdown lifetime issue.

## Sensors

`listSensors()` exposes `temperature` and `accelerometer`. `getSensorInfo()`
describes their types, units, and string formats without reading hardware.

| Key | Type | Units | `readSensor()` value |
| --- | --- | --- | --- |
| `temperature` | Integer | Degrees Celsius (`C`) | Signed decimal, for example `-12` |
| `accelerometer` | String | Thousandths of standard gravity (`mg`) | JSON object, for example `{"x":1,"y":-2,"z":1000}` |

Accelerometer axes retain the SDK's signed integer values; `1000` represents
one standard gravity. JSON uses `x`, `y`, and `z` numeric members and is independent
of the application's locale. The SDK 4.26 sensor API reports these units for all
supported products. Metadata does not imply that a card has an accelerometer:
the read checks support and returns `{}` when it is absent.

The existing read API logs SDK failures and returns `0` for temperature or `{}`
for acceleration. A temperature value of `0` alone therefore cannot distinguish
a valid reading from a failed read. Metadata requests for unknown keys raise
an argument error.

## Validation and development status

Use the SDK-free [non-hardware suite](tests/non_hardware/README.md) and presets
above for portable regressions. Native SDK builds and isolated module discovery
are separate checks; module loading alone does not open a card or validate RF.

The [hardware tools](tests/hardware/README.md) cover capability/readback,
counter/format validation, finite captures, paired tones, and PPS. Capability
queries and counter checks do not need an external RF generator or analyzer.
Tone/full-duplex checks need a documented RF path; calibrated absolute power or
sensitivity needs suitable calibrated equipment. Follow the
[post-update procedure](tests/hardware/POST_UPDATE_VALIDATION.md) for the newer
ownership, shutdown, configuration, and buffering changes.

The [development release notes](docs/RELEASE_NOTES.md) distinguish pending update
work, local build/sanitizer checks, and the earlier reported hardware matrix.
They are an unreleased draft. Final candidate review, compatible-host TSan,
relevant hardware follow-up, and evidence collection remain release checks.

## License

- https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE
