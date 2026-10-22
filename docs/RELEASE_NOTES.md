# Development update release notes

Unreleased draft, 2026-10-21. Integration status checked against master
`7d295b4551004ea411fab550017e9b93d6cac6d8`, through #145. The TX ownership,
diagnostic containment, source decomposition, private device boundary, legacy
SoapySDR compatibility, and production/fixture cleanup are merged. Later work
adds RF/session interaction coverage, documented SDK lifetime evidence, and an
optional concrete-driver integration test with a scripted SDK.

Draft card/API contributions #80–#86 are excluded from the current software
completion pass. They remain pending and are not included in these capability
claims. See the [software follow-up](validation/2026-10-21-software-followup.md)
for the exact checked source and remaining gates. Pin the final release commit
and refresh its validation matrix before publishing a release. The
[2026-10-22 hardware results](validation/2026-10-22-hardware-results.md) record
the matrix run on `7d295b4`, with remaining qualification gaps explicit.

## Behavior changes and fixes

- Discovery validates SDK results and constructor keywords before opening a
  card. Logical channels use the SDK parameter table, including non-contiguous
  handle maps. Invalid channels, names, numeric values, and unsupported stream
  options produce explicit errors.
- Stream setup rejects nonempty per-stream keyword maps before SDK access.
  The driver advertises no stream options; previously such values, including
  the unused `bufflen`, were silently ignored. Constructor-only options must
  be supplied when opening the device.
- RX uses an owned worker/session and bounded sample queue. Partial reads retain
  sample order and timestamp phase. Actual fractional rates, RF/system counter
  conversion, and signed nanosecond overflow are checked. Worker failures wake
  readers and preserve hardware ownership until stop succeeds.
- TX accepts CS16 and normalized CF32 in whole-MTU requests. CF32 clips finite
  out-of-range values and infinities; NaN maps to zero. Write deadlines cover
  PPS, busy-buffer, and queue-full waits. Errors after accepted packets return
  that accepted prefix so callers can advance once and retry the suffix.
- TX deactivation closes admission, cancels waiting callers, and drains admitted
  calls. The integrated ownership stack retains completion bookkeeping and SDK
  blocks independently of the device and joins the PPS worker before teardown.
  Its configurable `tx_buffer_count` accepts 1–30000; the compatibility default
  remains 30000 pending hardware throughput/latency measurements.
- Gain, calibration, RF port, settings, sensor, clock, and time APIs validate
  their supported choices and report backend operation/status. Configuration
  changes that affect RX rate/timing require successful RX deactivation first.
  See the [main README](../README.md) for individual API contracts and the
  [channel audit](ChannelValidationAudit.md) for the audited entry points.
- Stream errors and timeouts use SoapySDR status results. Integrated diagnostic
  containment keeps a failing log sink from replacing a stream result or
  accepted TX prefix. Control APIs retain throwing error boundaries; sensors
  retain their documented legacy fallback values; C callbacks contain exceptions.
- RF setters and activation use validated fresh SDK readback; unused retained
  rate/bandwidth snapshots have been removed. Scripted RF/session tests cover
  RX ownership rejection, failed-stop retention, coupled-clock updates, and
  fresh fractional-rate timestamps on restart. Physical RF continuity is a
  separate hardware check.
- The build uses C++17, target-scoped SDK linkage/warnings, configurable
  toolchains, SDK-free test presets, sanitizer presets, and isolated installs.
  Examples use finite operation and explicit cleanup, with a noninteractive
  hardware API diagnostic available under `tests/hardware`.

- The optional [concrete-driver test](../tests/sdk_integration/README.md)
  compiles the real device, registry adapter, backend, and lifecycle owners
  against installed SDK headers without linking the SDK library or opening
  hardware. It supplements the detailed portable fixtures; it does not emulate
  SDK transport, physical PPS timing, or multiple-device SDK ownership.

## Compatibility and remaining limits

- Minimum dependencies remain CMake 3.21, C++17, SoapySDR 0.4.0, and Sidekiq SDK
  4.26.0. Master includes the 0.4 gain-range and relocatable module-install fixes in
  [#114](https://github.com/epiqsolutions/SoapySidekiq/pull/114) and
  [#115](https://github.com/epiqsolutions/SoapySidekiq/pull/115). Older range APIs
  report bounds without step metadata. This is not a claim that every
  intermediate SoapySDR/SDK version or platform has been tested.
- One stream per direction and one logical channel per stream are supported.
  Arbitrary partial-block TX staging, nonzero absolute-time TX scheduling, and
  finite-burst activation are unsupported. Next-PPS activation is distinct from
  an absolute timestamp request; follow the documented flag/time combinations.
- The SDK PPS start call can block until an edge. Deactivation joins that worker
  and therefore waits for SDK startup to return, whether by an edge or an error.
  The SDK release history records missing-PPS timeout handling; this series has
  not established a universal return bound or caller-controlled cancellation.
  Writer timeout/cancellation does not cancel SDK startup. A process timeout is
  a diagnostic outcome, not proof of successful stream cleanup. See the
  [SDK lifetime review](SDK_LIFETIME.md) for the reviewed contracts and limits.
- Retained TX storage protects a completion that arrives late. Delivery of every
  callback, release of contexts whose callbacks never arrive, and an SDK
  callback-quiescence barrier remain unresolved. Stop/unregister success alone
  is not treated as a documented callback barrier.
- Process-wide SDK/card ownership is being addressed by draft
  [#85](https://github.com/epiqsolutions/SoapySidekiq/pull/85). Two-card lifetime
  validation remains pending. Do not infer multi-card teardown safety from
  separate single-card processes.
- Pending draft [#80–#86](https://github.com/epiqsolutions/SoapySidekiq/pulls) also cover
  per-handle gain limits, named RF/attenuation APIs, channel metadata, RX loss
  reporting, and discrete profile rates. Review and audit their integrated
  behavior before making broader capability claims.
- The historical 30000-block TX pool is about 1.83 GiB of payload/header storage
  at the default 16380-sample block size, before allocator/bookkeeping overhead.
  Smaller counts trade memory for buffering; no new default has been justified
  by sustained hardware measurements.

## Validation record

| Environment | Evidence and scope |
| --- | --- |
| Linux x86, GCC 11 / Clang 14, SDK 4.26, SoapySDR 0.8.1 | The software follow-up checks the full module build/link, isolated install, and loading. All 485 portable cases pass with GCC, Clang, and ASan/UBSan. Sixteen concrete-driver cases pass with GCC, Clang, and ASan/UBSan. Hardware behavior and the final release tree remain separate. |
| Linux x86, GCC 11, SDK 4.26, SoapySDR 0.4.0 | The real tagged library, ABI `0.4-2`, builds/loads the module and runs all sixteen scripted concrete-driver cases. Earlier #114/#115/checkpoint evidence covers relocation and staging. No hardware opened with this SoapySDR version. |
| AArch64 GCC 11.4, SDK 4.26, target-built SoapySDR | Full cross-build/link/install and ELF checks passed during the series. Target execution and final release-tree validation remain separate. |
| TSan | All 485 portable cases and 16 concrete-driver cases pass locally with Clang 18.1.8, per-process ASLR disabled, and no reports/suppressions. The [follow-up](validation/2026-10-21-software-followup.md) pins this source; the [TSan record](validation/2026-10-21-tsan.md) describes the runtime setup. This does not instrument the proprietary SDK or establish hardware callback guarantees. |

The user reported the earlier hardware campaign complete on 2026-10-16 with
SDK 4.26, SoapySDR 0.8.1, GCC 11.4, and external GPS-aligned PPS. Its original
candidate and procedure are preserved in the
[hardware integration plan](../tests/hardware/INTEGRATION_TEST_PLAN.md).
Run-specific raw logs are not included in these release notes.

| Historical hardware | Recorded coverage and limits |
| --- | --- |
| PCIe `47479` and M.2-2280 `8N55` | Paired RF/full-duplex campaign reported complete. Initialize M.2 before starting PCIe TX: the opposite order can also fail in direct SDK applications. Some high M.2 output settings distorted the SDK-generated tone. Preserve the measured operating settings in run evidence. |
| NV100 `9YAA` | RX B1/J2 with TX A1/J1 passed full duplex at 1.92 MS/s. Tested profiles were 1, 1.92, and 10 MS/s; 2 MS/s was rejected. J2 TX produced no measured tone even in the direct SDK check and remains unverified. |
| Other models/paths | Z4 was absent, X4 disconnected, and absolute RF power/splitter calibration was not established. Unrecorded PPS sources and model/path combinations require their own evidence. |

Later production changes require the
[post-update hardware follow-up](../tests/hardware/POST_UPDATE_VALIDATION.md).
Before release, record the final commit, complete review/integration, rerun
normal/sanitizer and native/ARM checks, attach hardware evidence, audit the
final documentation, and account for useful code/tests before retiring old
branches. Keep unsupported or skipped combinations explicit.

The [2026-10-22 hardware record](validation/2026-10-22-hardware-results.md)
adds source-pinned results for 58 matrix commands and three corrected passive
diagnostic reruns (64 checks). It does not supersede the remaining SDK contract,
pool-measurement, same-process, or target qualification requirements.
