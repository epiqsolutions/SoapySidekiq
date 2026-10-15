# Integration validation

## Candidate and software results

On 2026-10-15, `test/non-hardware-integration` at `93b4113` (through PR #24)
was combined with `master` at `1fce6ff` (including PRs #20 and #23).
The Settings.cpp conflict preserves both the discovery-cache removal and
the TX-enabled callback removal. The RX and TX test fixtures use the new PPS
state, and two permanent production-write tests cover failed and successful
PPS startup results.

Validation on GCC 11.4.0:

- Normal suite: **109 individual cases passed**, in 11 CTest executables.
- ASan/UBSan, including float-cast-overflow: **109 individual cases passed**.
- Ruff: the nine examples and three rewritten hardware validators passed.
- Python syntax: all 13 example/hardware programs parsed.
- Streaming.cpp: syntax-only compile passed with installed SDK 4.23 headers.
- Whitespace check passed.

Reproduce the non-hardware checks from this branch:

```sh
cmake -S non_hardware_tests -B build/non_hardware -DCMAKE_BUILD_TYPE=Debug
cmake --build build/non_hardware --parallel 4
non_hardware_tests/run_suite.sh build/non_hardware
cmake -S non_hardware_tests -B build/non_hardware-asan \
  -DCMAKE_BUILD_TYPE=Debug -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-asan --parallel 4
non_hardware_tests/run_suite.sh build/non_hardware-asan
```

The full driver configure was attempted with `/home/dhelm/sidekiq_sdk_current`
and failed because that installation is SDK 4.23 and lacks `sidekiq-config`.
A successful build against SDK 4.26+ is still required. The syntax-only check
above does not establish that the full module builds or links.

## Pending hardware validation

No hardware tests have been run on this combined candidate. Before merging
the integration branch into master, build/install it with SDK 4.26+ and record
the following results. Use supported low, nominal, and high sample rates for
stream checks; explain capability-based skips.

| Area | Required checks | Result |
| --- | --- | --- |
| Installation/discovery | Module discovery, `SoapySDRUtil --check`, `--find`, `--probe` | Pending |
| Channel mapping | Single-channel device and multi-handle/Z4 topology where available | Pending |
| RX samples/time | CS16 and CF32 counters, RF/system timestamp continuity, finite captures | Pending |
| RX lifecycle | Repeated activate/deactivate, partial reads, immediate cleanup after PPS activation | Pending |
| TX | CS16/CF32 loopback tone level/frequency; product-specific sample alignment | Pending |
| Cross-direction | Full duplex and RX retuning with a supported channel/RF configuration | Pending |
| PPS | Timed RX/TX starts/stops and pending-start cleanup, internal/external sources where available | Pending |

For each run, save the tested commit, command, card model/serial, channel and
topology, SDK/SoapySDR versions, format/rate, RF attenuation/path or PPS setup,
and observed result. The bounded programs in this directory and
`examples/python` are starting points; running them alone does not cover the
entire matrix. Keep RF setup and expected behavior specific to the card.
