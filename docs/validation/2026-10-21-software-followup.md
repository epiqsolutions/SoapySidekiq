# Software validation follow-up, 2026-10-21

Checked source: master `7d295b4551004ea411fab550017e9b93d6cac6d8` through
#145, plus this PR's mechanical cleanup and documentation. This follows the
[earlier audit](2026-10-21-software-closeout.md); it does not turn a software
audit or scripted run into hardware/SDK qualification.

## Fixes and coverage added after the earlier audit

- #137 rejects unsupported constructor clock/time source values before SDK
  initialization, preserving supported case-insensitive names. #138 covers
  optional topology/source setup failures and exactly-once single-device rollback.
- #139 establishes a working local portable TSan run and a fifth CI job. The
  [TSan record](2026-10-21-tsan.md) explains the matched runtime/header selection
  and per-process address-layout setup. No sanitizer suppressions were added.
- #140 instruments the concrete driver and scripted SDK with optional TSan and
  tests completion on another thread during actual device teardown.
- #141 invokes all 28 implemented scalar-channel methods through 672 invalid
  requests with absent directions/boundary/oversized indices. #142 exercises
  actual PPS startup, writer cancellation, and join/stop/unregister/exit order.
  Its review fix uses deterministic phase observations rather than assuming
  an async task started because its future timed out. Hooks compile only into
  the scripted target; the normal module has no hook references or definition.
- #143 rejects unsupported per-stream keywords before SDK reads/setup changes,
  including formerly ignored `bufflen`. #144 covers all seven token-taking
  methods and transfer/control rejection. #145 tests failed RX/TX stops,
  retained ownership/admission, close retries, fresh-rate RX restart, and TX
  host submissions across the RX sequence. These are host payload/control
  checks, not physical RF continuity measurements.
- This PR corrects stale RF-cache wording and the MTU trace spelling, names the
  callback's local owner `context`, and removes an unread fixture-only index.
  It changes no RF, ownership, timeout, or admission policy.

## Verification

| Check | Result and limits |
| --- | --- |
| Portable suite | All 485 cases pass GCC 11, Clang 14, GCC ASan/UBSan, and matched Clang 18 TSan. Public CI runs the same five jobs including analysis and uploads counted logs. |
| Concrete driver | All 16 native cases pass GCC, Clang, ASan/UBSan, actual SoapySDR 0.4, and Clang 18 TSan. SDK headers/build metadata are required, but no SDK library is linked; unexpected SDK calls fail closed. The proprietary SDK and linked SoapySDR library are not instrumented. |
| Native SDK modules | GCC/Clang build, isolated install, and `--check=sidekiq` loading pass with SDK 4.26 / SoapySDR 0.8.1. The real SoapySDR 0.4 module also builds/loads. Loading does not open a card. |
| ARM | AArch64 cross-build/link/install and installed ELF checks pass. Actual target execution remains outside these software build results. |
| Hygiene | All 36 production C++ files match clang-format 14 and have file-level documentation. Public/internal Doxygen logs are empty; configured native clang-tidy covers all 17 root module units without findings. Relative Markdown file links resolve; external URLs/anchors are outside that check. |
| Artifact/install policy | No generated binaries/build products are tracked. The earlier [checkpoint](2026-10-21-integration.md) records relocation/staged-prefix preservation; install rules have not changed since it. Newer normal isolated installs/load checks pass. |

Node-local logs remain under ignored `build/`: `software-followup-portable-*`,
`software-followup-concrete-*`, `software-followup-modules.log`,
`software-followup-arm-legacy.log`, and `software-followup-analysis.log`.
Doxygen warnings remain under `build/{api-docs,internal-docs}/warnings.log`.
These local logs are not remotely attached evidence; CI keeps separate
portable artifacts. The [concrete target instructions](../../tests/sdk_integration/README.md)
describe native sanitizer setup; [CONTRIBUTING](../../CONTRIBUTING.md) describes
the ordinary test/build/documentation commands.

## Remaining plan boundaries

| Item | Required next action |
| --- | --- |
| Callback barrier and undelivered contexts | Obtain an applicable SDK contract establishing when the SDK no longer accesses accepted blocks/user contexts, including errors. [Prepared vendor questions](../SDK_LIFETIME.md#clarification-request-for-epiq) distinguish quiescence, delivery, and reclamation. Current code retains accepted storage; delivered-late callbacks are covered. |
| PPS startup bound/cancellation | Establish the SDK's applicable timeout/cancellation contract and hardware outcomes. Writer cancellation is implemented; it does not cancel the blocking SDK start. The worker remains joined before device release. |
| Draft #80–#86 | Explicitly excluded by user request. Shared SDK/card ownership, additional API/capability contributions, and discrete profiles require their own review/integration; do not duplicate or count them complete. |
| TX default and hardware matrix | The bounded configurable pool and host memory-cost documentation are implemented. Choosing a smaller default requires hardware throughput/latency results. Hardware validation runs separately; preserve its exact source/commands/artifacts and capability skips. |
| Final release and branches | Review/integrate work, apply the hardware results, repeat affected target/software checks, and retain old branches until useful content and qualification evidence are accounted for. No branch retirement or release publication was performed. |

Keep these unresolved requirements visible. There is no unconditional callback
reclamation or real-SDK startup-cancellation guarantee established by the passing
software tests above.
