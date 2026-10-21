# Software plan closeout, 2026-10-21

Checked source: master `d5424eac7fb93a603a237ff7fbd92ac1ba53421c`, through
#135, plus this PR's documentation and comment-only cleanup. Modified C++ files
have identical non-comment tokens. This completes the available software
cleanup/audit pass, subject to independent review; it does not complete the
hardware and SDK-dependent release gates below.

By request, draft PRs #80–#86 are excluded. They were neither modified nor used
as prerequisites for this pass. Audit their additions separately if integrated.

## Results

| Check | Result and scope |
| --- | --- |
| Portable suite | 482 individual cases pass with GCC 11, Clang 14, and GCC ASan/UBSan. Clean builds passed at `91c0f91`; affected fixtures were rebuilt and all cases rerun after #135. No compiler warnings in these logs. |
| Native module | SDK 4.26 / SoapySDR 0.8.1: GCC 11 and Clang 14 build, link, isolated install, and driver-load checks pass. Clean builds passed at `91c0f91`; affected units were rebuilt after #135. No compiler warnings. `--check` loads the module without opening a card. |
| Concrete-driver integration | Three cases pass in each of GCC, Clang, GCC ASan/UBSan, and actual SoapySDR 0.4.0 configurations. These compile the real device and lifecycle code with scripted SDK symbols, with no SDK library on any link line and no unresolved SDK symbols. See the [target's scope](../../tests/sdk_integration/README.md); immediate startup is exercised, not physical PPS or transport. |
| Legacy library | The module builds, installs, and loads with the real SoapySDR 0.4.0 library, ABI `0.4-2`, and SDK 4.26. No hardware opened. |
| ARM | AArch64 cross-build/link/install passes and the installed module is ELF AArch64. No target execution or ARM hardware result. |
| Static analysis | Configured clang-analyzer/bugprone checks cover all 17 root module implementation units using the native Clang compile database, with no reported findings. Generated/filtered diagnostic counts are distinct from compiler warning counts. This is the conservative configured check set. |
| Formatting and documentation | All 36 production C++ source/header files match clang-format 14 and have file-level Doxygen blocks. Public and internal Doxygen report zero warnings. Tracked Markdown relative file links resolve; external URLs and section anchors are not checked. |
| API boundaries | The [channel/request audit](../ChannelValidationAudit.md) accounts for all 28 implemented scalar-channel methods and records flag, token, numeric, setting, and error boundaries with regression references. Inherited defaults and draft overloads are outside that implemented-method inventory. No new confirmed validation defect was found. |
| Source/artifact inventory | All 17 root implementation units occur in the native compile database. The production scan finds no disabled `#if 0` blocks, obsolete debug macros, TODO/FIXME markers, or commented-out statements. Tracked artifacts remain source files; this is not proof that every retained member/branch is necessary. |

Earlier [checkpoint evidence](2026-10-21-integration.md) pins custom-prefix
relocation with spaces, local DESTDIR staging under `/usr` and `/usr/local`, and
unrelated-file preservation at `be1364e`. Install rules have not changed since
that check; those layout exercises were not repeated in this pass. Normal
isolated installs and module loading above use the newer source.

## Changes found by the cleanup/audit

- #128 adds real RX-session/RF-setter interaction tests; #129 gives TX context
  and ring-index state descriptive names.
- #131 records installed SDK header/manual/release-history evidence for PPS
  waits and callback lifetime without inventing a cancellation or drain guarantee.
- #132 removes constructor-only RX resolution state. #133 removes four RF
  snapshots with no production readers while retaining mapped constructor and
  post-write SDK validation. Regression mutations exercise those validations.
- #134 adds the optional real-device integration target. #135 removes the
  obsolete header buffer constant and uses typed fixture-local counts.
- This PR removes redundant constructor/stream comments, clarifies the receive
  timeout's stop-observation purpose, fixes the pool allocation comment, refreshes
  stale formatting/warning guidance, and updates release/request-validation
  documentation. It changes no executable C++ tokens.

## Reproduction and evidence

Use the portable commands in [CONTRIBUTING](../../CONTRIBUTING.md), the
[scripted integration instructions](../../tests/sdk_integration/README.md),
the [ARM guide](../AARCH64_BUILD.md), and the exact checked revision above.
Run public/internal Doxygen and the configured native clang-tidy command from
the earlier checkpoint. SDK/SoapySDR paths and cross-toolchain configuration
must match the intended environment.

Node-local ignored logs are `build/software-closeout-{gcc,clang,asan}.log`
for the clean portable runs, `software-closeout-native-{gcc,clang}.log`,
`software-closeout-legacy.log`, and `software-closeout-arm.log` for the clean
module/cross builds, and `software-closeout-final-{gcc,clang,asan,native,compat,
integration,tidy}.log` for the post-#135 rebuilds/runs/analysis. Public/internal
Doxygen warnings are under `build/{api-docs,internal-docs}/warnings.log`.
These node-local paths are not remotely attached raw artifacts. CI retains its
own portable logs; the optional proprietary-header integration target is a
local check and is not exercised by SDK-free public CI.

## Remaining gates

| Item | Next evidence needed |
| --- | --- |
| New hardware qualification | Run the [post-update procedure](../../tests/hardware/POST_UPDATE_VALIDATION.md) on a pinned integrated candidate. Record card/path/PPS source, commands, outcomes, and raw artifacts. Earlier user-reported hardware results do not qualify later changes. |
| TSan | Run on a compatible native host. This node previously could not start the runtime because of an unexpected memory mapping; compilation alone is not a pass. |
| SDK lifecycle guarantees | Establish callback quiescence and reclamation for callbacks that never arrive, and an observed/documented PPS startup return bound or caller-controlled cancellation if required. See [SDK lifetime evidence](../SDK_LIFETIME.md). Stop/unregister success is not assumed to prove a callback barrier. |
| TX pool default | Hardware throughput/latency and back-pressure measurements before replacing the 30000-block compatibility default. The bounded constructor override is already implemented. |
| Multi-device/card behavior | Draft #85 and real multi-card lifetime validation are outside this pass. Separate single-device tests cannot establish process-wide SDK ownership safety. |
| Release candidate | Integrate reviewed work, pin the final source, repeat affected software/ARM checks, run on supported targets, and refresh the documentation/hardware matrix. Keep experimental branches until their useful content and release evidence are accounted for. |

The release remains unreleased. Remaining broad plan checkboxes covering all
inherited/future public APIs, all SDK shutdown outcomes, or a final hardware
release are not satisfied by this scoped software audit.
