# Hardware results, 2026-10-22

The operator completed the established 58-command hardware matrix on the driver
at `7d295b4551004ea411fab550017e9b93d6cac6d8` (through #145).
On 2026-10-22, the software session read the operator's summary and command logs
from `dave@192.168.2.151:/home/dave/SoapySidekiq`, verified their recorded commit
and exit statuses, and inspected the three final API diagnostic JSON reports.
This is evidence review of that run, not another hardware execution.

## Evidence and source identity

- Operator summary: `logs/master_20261022_hardware_validation_summary.md`.
- Each command, timestamp, logged HEAD, and exit status is retained in
  `logs/*master_20261022*.log`. The replay script is
  `build/master-20261022-rerun.py`; it depends on prior replay metadata/logs.
- Captures, paired child logs, and JSON reports are under
  `build/integration-evidence/master_20261022/` on that node.
- The [evidence inventory](2026-10-22-hardware-evidence.json) preserves exact
  commands, log paths, SHA-256 hashes, exit statuses, and final diagnostic totals.
  Raw outputs/captures remain Git-ignored node-local files, not public attachments.
  The inventory does not make those files remotely durable; retain them for review.

All 58 matrix commands exited 0. Three additional build/module commands also
exited 0. Three initial passive API diagnostics exited 1; they are preserved as
failures, separately from the matrix. Their cause and successful replacement
runs are described below. Thus the wildcard log inventory is broader than
58 successful matrix commands; it must not be reported as failure-free overall.

## Setup and observed coverage

The production module was built with GCC 11.4.0 and Sidekiq SDK 4.26.0, installed
under `build/master-20261022-install`, and selected through `SOAPY_SDR_ROOT`.
The module-info log identifies only that isolated module, version `7d295b4`,
with SoapySDR library 0.8.1 / ABI `0.8-3` (reported API `0.8.200`).

| Card | Channels used and host scale |
| --- | --- |
| MPCIE C rev C, `47479` | RX0/TX0; CS16 full scale 2047 |
| M.2-2280 rev B, `8N55` | RX0/TX0; CS16 full scale 2047 |
| NV100 rev C, `9YAA` | Logical RX0=A1/J1, RX1=A2/J2, RX2=B1/J2; TX0=A1/J1; CS16 full scale 32767 |

The operator reports reciprocal splitter paths and a GPS-aligned external PPS
box. The tests observe samples and PPS behavior; cable loss, absolute RF power,
and GPS-to-UTC phase were not independently measured.

| Check family | Recorded result and scope |
| --- | --- |
| Identity | Discovery and three card probes passed. |
| Receive | CS16 counter and RF/system timestamp continuity, CF32 conversion, repeated activation, partial reads, and finite captures passed on the documented channels. Counter rates were 1/2/10 MS/s for PCIe/M.2 and 1/1.92/10 MS/s for NV100 B1/J2. |
| One-way RF | CS16/CF32 tone frequency/level/clipping assertions passed in both PCIe↔M.2 directions at 2 MS/s and supported PCIe↔NV100 paths at 1.92 MS/s. CS16 checks passed at 1 and 10 MS/s in both directions for each pair. PCIe→NV100 RX A1, A2, and B1 passed at nominal rate. |
| Full duplex | PCIe↔M.2 at 2 MS/s and PCIe↔NV100 RX B1/J2 + TX A1/J1 at 1.92 MS/s passed with distinct measured tones and no clipping. |
| RX retune | PCIe RX retuned 1.0000→1.0001 GHz while a separate M.2 or NV100 transmitter process remained active. Both before/after tones passed. This does not establish uninterrupted TX samples or same-device RX/TX coupling. |
| External PPS | Timed RX/TX starts/stops, pending-start cleanup, and stream reuse passed on M.2, PCIe, NV100 A1/A1 and B1/A1. First-write waits were 0.300, 0.295, 0.279, and 0.261 seconds respectively. Edges were present. |

The PCIe→M.2 10 MS/s capture used a four-second receive duration to avoid the
previously measured short-overlap test limit. It passed with 2.442 seconds of
overlap, +249,633.8 Hz peak versus +250,000 Hz expected, 79.6 dB tone/noise,
and zero clipping. Exact settings and program assertions remain in the logs.

## Passive diagnostic correction

The original diagnostic attempted LO readback before tuning. All three cards
returned the SDK's documented untouched-LO `-ENODATA` (-61), and grouped RF
readback prevented independent sample-rate/bandwidth checks. Those initial
failures are diagnostic behavior, not successful tests.

The final correction omits LO reads and checks sample rate and bandwidth
independently. Its Python source was subsequently committed as
`e52796e624b3b40d3ffb7a7be35300d89ffe6050` in
[PR #147](https://github.com/epiqsolutions/SoapySidekiq/pull/147).
During these reruns the working Python source contained that correction, while
logged HEAD and the installed production module remained `7d295b4`.

The three final `*_no_lo_read.json` reports show M.2 16, PCIe 16, and NV100 32
checks passed: **64 passes, zero failures, zero skips**. Their exact commands and
hashes are in the inventory. Earlier skip-based experiments remain historical
and do not describe the final diagnostic behavior.

## Unfinished qualification

This run completes the stated matrix on the listed hardware. It does not
complete every check in the [follow-up procedure](../../tests/hardware/POST_UPDATE_VALIDATION.md).
The operator's summary explicitly leaves these outside this run:

- Same-process shared-SDK ownership (draft #85 remains excluded).
- Controlled TX pool-count/RSS/throughput/latency sweeps needed to choose a new
  pool default, and hardware stop-error recovery.
- Missing-PPS diagnostics and the SDK's startup timeout/cancellation contract.
- Callback/data-access quiescence and reclamation of accepted contexts when
  callbacks never arrive; finite successful observations do not prove a barrier.
- ARM hardware, internal PPS, calibrated RF power/path loss, X4 (disconnected),
  and NV100 J2 TX RF (operator set aside the setup/cabling gap).
- The known reversed-start SDK crash sequence, which was not exercised.

Physical TX continuity across same-device RX configuration changes remains
unqualified by this paired-process retune result. Keep those requirements open,
retain the [SDK questions](../SDK_LIFETIME.md#clarification-request-for-epiq),
and repeat affected tests after production changes. These results qualify the
pinned module and listed cases, not an unspecified future release tree.
