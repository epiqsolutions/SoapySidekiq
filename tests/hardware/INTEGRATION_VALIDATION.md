# Integration validation record

## Candidate, build, and node

Tested on 2026-10-15 on `test/non-hardware-integration` at exact commit
`4db75a965421a9ac79e2cce54fecf80bedfa79c8`. Fetch found no newer
integration-branch commits. The branch has not been merged into master.

Installed software: Sidekiq SDK **4.26.0** at
`/home/dave/sidekiq_sdk_v4.26.0` (`sidekiq-config --version`), SoapySDR
**0.8.1** (API 0.8.200, ABI 0.8-3), GCC **11.4.0**. The actual driver
configured, built, linked, and installed into the isolated prefix:

```sh
cmake -S . -B build/integration-driver -DCMAKE_BUILD_TYPE=Debug \
  -DSIDEKIQ_SDK_DIR=/home/dave/sidekiq_sdk_v4.26.0 \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/integration-install"
cmake --build build/integration-driver --parallel 4
cmake --install build/integration-driver
SOAPY_SDR_ROOT="$PWD/build/integration-install" SoapySDRUtil --info
SOAPY_SDR_ROOT="$PWD/build/integration-install" SoapySDRUtil --check=sidekiq
```

`SoapySDRUtil --info` reported only
`build/integration-install/lib/SoapySDR/modules0.8-3/libSidekiqSupport.so`
(version `4db75a9`); the installed system module was excluded. The CMake
cache confirms the SDK and prefix. Normal and ASan/UBSan non-hardware suites
each passed **109/109 individual cases**. After moving the suite from
`non_hardware_tests/` to `tests/non_hardware/` on 2026-10-16, fresh builds
and both suites again passed **109/109** using:

```sh
cmake -S tests/non_hardware -B build/non_hardware-relocated -DCMAKE_BUILD_TYPE=Debug
cmake --build build/non_hardware-relocated --parallel 4
tests/run_logged.sh relocated_nonhardware_normal_suite \
  tests/non_hardware/run_suite.sh build/non_hardware-relocated
cmake -S tests/non_hardware -B build/non_hardware-relocated-asan \
  -DCMAKE_BUILD_TYPE=Debug -DSOAPYSIDEKIQ_TEST_ASAN=ON
cmake --build build/non_hardware-relocated-asan --parallel 4
tests/run_logged.sh relocated_nonhardware_asan_suite \
  tests/non_hardware/run_suite.sh build/non_hardware-relocated-asan
```

Original outputs: `logs/20261015_normal_109.log` and
`logs/20261015_asan_109.log`; relocation reruns are in the labeled `logs/`
entries above.
Python syntax passed; Ruff was unavailable on this node. The full SDK 4.26
build replaces the earlier SDK 4.23 syntax-only driver check.

On 2026-10-16, the revised `install.sh` also configured, built, and installed
this commit into `build/install-script-prefix` with an explicit SDK path, then
into `build/install-script-autodiscovery-prefix` in a fresh build without an
SDK argument. The latter found `$HOME/sidekiq_sdk_current` via
`sidekiq-config` 4.26.0. `SoapySDRUtil --info` with each isolated prefix found
only that prefix's `libSidekiqSupport.so (4db75a9)`; `--check=sidekiq` passed
for the explicit-prefix install. Reusing the first build directory with
`SIDEKIQ_SDK_DIR` set to the SDK symlink correctly updated CMake's cached
`sidekiq-config` path. Evidence labels: `install_script_isolated_smoke`,
`install_script_isolated_load`, `install_script_isolated_check`,
`install_script_fresh_default_sdk_discovery`,
`install_script_autodiscovery_load`, and `install_script_sdk_env_reselect`.
The privileged system-wide install path was not run.

## Cards, RF path, and evidence

Isolated discovery/probe found:

| Serial | Model | Card | Channel/handle mapping | Host CS16 full scale |
| --- | --- | ---: | --- | ---: |
| `47479` | MPCIE C rev C, PCIe | 0 | RX0/TX0 → handle 0 | 2047 |
| `8N55` | M.2-2280 rev B | 1 | RX0/TX0 → handle 0 | 2047 |
| `9YAA` | NV100 rev C | 2 | RX/TX 0–2; channel 0 J1, 1/2 J2 | 32767 |

For the initial 2026-10-15 phase, the operator identified
`8N55 TX → 47479 RX` and `47479 TX → 8N55 RX`
through splitters, with **no added attenuators**. The operator also reports a
separate GPS-aligned box distributing external PPS to all three cards; the
GPS-to-UTC phase itself was not measured in this test. Only this pair was exercised
beyond discovery/probe in that phase. NV100 was then scope-skipped; its
subsequent 2026-10-16 validation is recorded below. No Z4 is installed. The
32767 host full scale of NV100/X2/X4/X40 must not be applied to these two
2047-full-scale cards. Soapy TX gain 0 maps to maximum SDK attenuation;
larger settings reduce attenuation. The values below are Soapy gain settings,
not calibrated RF power. Splitter loss and absolute output power were not
measured.

Every initial-phase hardware test used `tests/run_logged.sh LABEL COMMAND...`,
an outer `timeout` (10–30 s), and 200,000 µs per stream read where applicable.
The wrapper saves UTC time, exact command, tested Git commit, output, and
exit status in a unique `logs/*.log`. `out.txt` is a stable symlink to the
append-only `logs/current.log`. Binary captures and child-process logs are in
`build/integration-evidence/`. The paired tests use **one process per card**;
`8N55` initializes first. Set `SOAPY_SDR_ROOT=$PWD/build/integration-install`
for every Soapy command.

## Hardware matrix

Each PASS below is a **program assertion** on process status, sample content,
timestamps, FFT frequency/level, or clipping. Card identification comes from
probe output; physical wiring comes from the operator. No visual spectrum
observation is counted as an assertion.

| Area | Configuration and result | Evidence label in `logs/` |
| --- | --- | --- |
| Installation/discovery | Isolated `--info`, `--check=sidekiq`, `--find`, and per-serial `--probe` loaded the candidate and mapped all three cards. Z4 absent. | `module_identity_*`, `module_check_*`, `discover_cards_*`, `probe_8N55_*`, `probe_47479_*`, `probe_9YAA_*` |
| RX counters/time | `8N55` CS16 and CF32 at 1, 2, 10 MS/s: 10 reads × 1018 counter samples; RF and system timestamp continuity passed. `47479` CS16/CF32 at 2 MS/s passed. | `rx_counter_*` |
| RX lifecycle | Three activation/deactivation cycles, 10 blocks/cycle, 2 MS/s passed on both cards. | `rx_loop_*` |
| Partial reads | 20 requests of 257 samples, below MTU 1018, passed counter and RF/system timestamp phase checks on both cards at 2 MS/s. | `rx_partial_*` |
| Finite capture | `8N55` CS16 and CF32, 10 blocks at 2 MS/s, produced exactly 40,720 and 81,440 bytes. | `rx_capture_*` |
| One-way TX | 1 GHz LO, +250 kHz tone, 2 MS/s, `8N55` CS16/CF32 TX setting 80 → `47479` RX passed: +250.61/+250.49 kHz; 27.6/41.1 dB tone-to-noise, no clipping. RX gain 0/20 respectively. | `rf_8N55_to_47479_cs16_txfirst_gain80_*`, `rf_8N55_to_47479_cf32_gain80_*` |
| Reverse TX | `47479` CS16/CF32 TX setting 80 → `8N55` RX passed: +249.39/+249.27 kHz; 30.7/44.7 dB tone-to-noise, no clipping. RX gain 0/20 respectively. | `rf_47479_to_8N55_cs16_rxfirst_gain80_*`, `rf_47479_to_8N55_cf32_rxfirst_gain80_*` |
| Low/high rate RF | CS16 passed in **both physical directions** at 1 and 10 MS/s, all at 1 GHz LO, +250 kHz offset, TX setting 80 and no clipping. `8N55` → PCIe: +250.61/+250.85 kHz, 46.3/40.3 dB tone-to-noise. PCIe → `8N55`: +249.39/+249.63 kHz, 49.3/41.9 dB tone-to-noise (RX gain 20). The reverse-path validator measured inside the actual RX/TX overlap. | `paired_8N55_to_47479_cs16_{1,10}msps_*`, `paired_47479_to_8N55_cs16_{1,10}msps_overlap_*` |
| Full duplex | **PASS** at 2 MS/s with separate card processes and a steady 6 s / 4 s overlap. Distinct +250/−300 kHz tones: `8N55` RX resolved the PCIe peer at −300.29 kHz, 14.9 dB tone-to-noise, peak 0.469 of full scale; PCIe RX resolved the `8N55` peer at +250.24 kHz, 26.9 dB tone-to-noise, peak 0.032 of full scale. Neither clipped; both processes sent and captured samples without stream errors. TX settings 68/89.75 and RX gains 20/40 on `8N55`/PCIe respectively. | `full_duplex_steady_overlap_2msps_*`; captures/child logs under `build/integration-evidence/duplex_steady_overlap/` |
| RX retune during peer TX | `8N55` TX process stayed active while `47479` RX stopped, retuned LO 1.0000 → 1.0001 GHz, and restarted. At 2 MS/s, 1 GHz TX LO, +250 kHz CS16 tone, TX setting 80, RX gain 20: measured +250.61 and +150.76 kHz, 42.7/41.5 dB tone-to-noise, no clipping. | `paired_retune_2msps_*`; captures/child log in `build/integration-evidence/retune/` |
| External PPS | **PASS** on `8N55` and `47479` at 2 MS/s, 1 GHz, CS16, RX gain 0, TX setting 0. Each detected advancing external PPS, returned 10,180 timestamped samples after timed RX activation, and completed bounded timed TX. The lifecycle check asserted timed RX/TX start and stop, timestamped RX read, one 16,380-sample TX write after the start gate, immediate RX cleanup with zero host reads, pending TX cleanup with zero submitted host samples, and successful immediate restart after both cleanups. Timed stops waited about 1 s for an edge; no stream error or timeout. The first `8N55` source check failed to see a pulse, but its retry and subsequent runs passed. GPS absolute phase was not independently measured. | `pps_source_*`, `pps_rx_*`, `pps_tx_*`, `pps_lifecycle_rx_cleanup_*` |

Representative reproducible commands:

```sh
export SOAPY_SDR_ROOT="$PWD/build/integration-install"
tests/run_logged.sh paired_retune_2msps timeout 20s \
  python3 tests/hardware/paired_retune_validate.py --rate 2e6 \
  --frequency 1e9 --tone-offset 250e3 --tx-gain 80 --rx-gain 20
tests/run_logged.sh paired_1msps timeout 18s \
  python3 tests/hardware/paired_tone_validate.py --tx-serial 8N55 \
  --rx-serial 47479 --tx-format CS16 --rate 1e6 --frequency 1e9 \
  --offset 250e3 --tx-gain 80 --rx-gain 20 --start-order tx-first \
  --tx-duration 3 --rx-duration 1 \
  --output build/integration-evidence/paired_1msps.cs16
```

## Initial-run failure investigation and assessment

Two runs initialized PCIe and M.2 processes in the opposite order. The
`8N55` process segfaulted inside `libsidekiq` `skiq_init`, with
`skiq_fpga_hal_read_rfic_spi_reg` / `ad9361_write_tx_output_port` in the
backtrace, before driver stream configuration. This was reproduced **without
SoapySidekiq** with the prebuilt apps from SDK **4.24.0, 4.25.0, and
4.26.0**: `tx_tone` ran on PCIe card 0 before `rx_to_file` started on M.2
card 1. In each version, RX exited 139 during initialization; TX was
intentionally bounded by `timeout` (exit 124). SDK 4.23 prebuilt apps were
not installed on this node. Thus the crash is an **SDK/card-initialization
defect or limitation**, not caused by this driver.
Initialize `8N55` first until Epiq provides a fix or supported sequence.
Repro command/output: `sdk_direct_pcie_tx_then_8N55_rx_*`,
`sdk425_direct_pcie_tx_then_8N55_rx_*`, and
`sdk424_direct_pcie_tx_then_8N55_rx_*`; corresponding child logs under
`build/integration-evidence/sdk{,425,424}_*.log`. Original
Soapy repro logs: `rf_8N55_to_47479_cs16_gain0_*` and
`rf_47479_to_8N55_cs16_gain60_rx0_*`; saved cores:
`build/integration-evidence/tx_init_8N55_2321864.core` and
`build/integration-evidence/rx_init_8N55_2331165.core`.

An October 16 SDK 4.26.0 C-app retry confirmed the ordering difference at
2 MS/s, 1.6 MHz bandwidth, and 1 GHz LO, with separate bounded processes.
When PCIe card 0 `tx_tone` (A1, attenuation index 80, +250 kHz) reached
streaming before `8N55` card 1 `rx_to_file` (A1, counter mode) launched,
`rx_to_file` again segfaulted during initialization (exit 139), before RX
streaming. Evidence: `sdk426_recheck_pcie_tx_then_8N55_rx`. In the reverse
order, `8N55` RX reached streaming first; PCIe TX then reached streaming
while RX remained active; the RX app verified its 64 MB counter capture and
exited 0. TX was intentionally stopped afterward (status 143). Evidence:
`sdk426_recheck_8N55_rx_then_pcie_tx`. Complete commands/results are in
`logs/`; child logs and capture are in
`build/integration-evidence/8n55_order_20261016/`. Counter mode verifies
initialization and stream integrity here, not RF tone reception. The crash
remains in the direct SDK C apps; this retry does not implicate SoapySidekiq.

The first `8N55` external-PPS source check saw no advancing pulse and failed;
a retry succeeded, as did timed RX, TX, and lifecycle runs. This is recorded
as a transient PPS detection/setup observation, with no reproducible driver
failure. At Soapy TX settings 0–60, the initial one-way tones were too weak to assert;
setting 80 yielded passing captures without clipping. The initial FFT
validator selected an unrepresentative near-DC window. It was corrected to
use the steady middle window and subtract DC: a **test analysis defect**, with
no production-driver edit. At 1/10 MS/s on the reverse one-way path, the initial short RX captures
placed their middle FFT windows before PCIe TX had established a steady tone. Even a 3 s RX capture
at 10 MS/s had its fixed middle FFT window before TX readiness: the logged
RX-to-TX marker delay was 1.508 s. The paired validator now measures a
window within the actual marker-based overlap; both rates passed on rerun.
This was a **test timing defect**. The failed logs and captures are preserved.
The first full-duplex validator used identical
tone offsets and could not distinguish local TX leakage from peer RF. It was
corrected to use distinct offsets. Short initial overlap and gain combinations
left one peer tone below the 10 dB threshold. A 6 s / 4 s bounded overlap with
separate gain settings passed both peer-tone assertions; this was a **test
setup/timing issue**, with no production-driver edit.
Failed gain trials, child logs, and captures remain under `logs/` and
`build/integration-evidence/`.

This initial-run assessment is superseded for RF levels by the corrected-switch
rerun below. The driver builds/links with SDK 4.26 and passes both 109-case suites,
isolated loading, RX checks, one-way CS16/CF32 RF in both directions, low/high
rate RF, full duplex with distinct peer tones, paired RX retuning, and
external-PPS RX/TX starts, stops, and cleanup on the two selected cards.
**The selected-card validation matrix is complete.** The direct SDK
initialization crash remains an external limitation, with a proven startup
order workaround; document that sequence for operators. NV100 is outside the
current scope, internal PPS was not exercised because the external GPS source
was selected, and absolute GPS/UTC phase was not independently measured. The
candidate is conditionally merge-ready for the selected `8N55`/`47479` scope
once this report is committed and reviewed; it is not an all-card signoff.

## 2026-10-16 corrected RF switch rerun

The operator reported that the RF switches had been moved to their intended
positions before this rerun. The exact switch positions and path loss were not
provided, so switch state is an **operator observation**, not an independently
verified assertion. The tested driver commit remained
`4db75a965421a9ac79e2cce54fecf80bedfa79c8`; isolated
`SoapySDRUtil --info` again loaded only the `4db75a9` module from
`build/integration-install`, with SDK 4.26.0 and SoapySDR 0.8.1. New history
labels begin `corrected_switch_` in `logs/`; captures and child logs are in
`build/integration-evidence/corrected_switch_20261016/`. Each paired run used
one process per card, initialized `8N55` first, and had an outer 20 s timeout.

| Check | Asserted result | Evidence label |
| --- | --- | --- |
| Discovery/isolation | Same module and card mapping found. | `corrected_switch_module_identity_*`, `corrected_switch_discovery_*` |
| RX counters and timestamps | `8N55` CS16/CF32 passed at 1, 2, 10 MS/s; PCIe CS16/CF32 passed at 2 MS/s. CS16 RF/system timestamps were continuous. | `corrected_switch_rx_*` |
| RX lifecycle/partial/finite | Three 10-block activation cycles on both cards; 20 partial reads per timestamp mode on both; `8N55` 10-block CS16/CF32 captures were 40,720/81,440 bytes. | `corrected_switch_loop_*`, `corrected_switch_partial_*`, `corrected_switch_finite_*` |
| Clean `8N55` TX → PCIe RX | At 1 GHz LO and 2 MS/s, TX gain **70**, RX gain **0**, +250 kHz tone: CS16/CF32 measured +250.61 kHz, 85.7/83.3 dB tone-to-noise, RMS 0.071/0.071 of full scale, crest 1.03/1.14, no clipping. CS16 at 1 and 10 MS/s measured +250.55/+250.85 kHz, 87.2/77.9 dB, crest 0.99/1.03, no clipping. | `corrected_switch_clean_forward_*` |
| PCIe TX → `8N55` RX | At TX gain **80**, RX gain **0**, 2 MS/s: CS16/CF32 measured +249.39/+249.27 kHz, 77.0/72.1 dB tone-to-noise, crest 1.02/1.08, no clipping. CS16 at 1 and 10 MS/s measured +249.45/+249.63 kHz, 93.8/95.9 dB, crest 1.00/1.03, no clipping. | `corrected_switch_reverse_{cs16,cf32}_*`, `corrected_switch_offline_crest_reverse_*` |
| Full duplex | **PASS** at 2 MS/s, distinct +250/−300 kHz peer tones, TX gain 70 on both cards, RX gain 0 on both. `8N55` RX peer tone −300.29 kHz, 52.7 dB tone-to-noise, peak 0.154 full scale; PCIe RX peer tone +250.24 kHz, 63.3 dB, peak 0.073; no clipping or stream errors. | `corrected_switch_full_duplex_2msps_*` |
| RX retuning during TX | **PASS** at 2 MS/s with `8N55` TX gain 70, PCIe RX gain 0. As RX LO moved 1.0000 → 1.0001 GHz, tone moved +250.61 → +150.51 kHz, 75.2/85.9 dB tone-to-noise, crest 1.04/1.11, no clipping. TX remained active. | `corrected_switch_clean_retune_2msps_*` |
| External PPS | **PASS** on both selected cards at 2 MS/s: source pulse detection, timed RX/TX starts and stops, zero-host-read RX cleanup, zero-submitted-sample pending TX cleanup, immediate restarts. | `corrected_switch_pps_lifecycle_*` |

A representative clean forward command was:

```sh
tests/run_logged.sh corrected_switch_clean_forward_cs16_2msps \
  env PYTHONDONTWRITEBYTECODE=1 \
  SOAPY_SDR_ROOT=/home/dave/SoapySidekiq/build/integration-install \
  timeout 20s python3 tests/hardware/paired_tone_validate.py \
  --tx-serial 8N55 --rx-serial 47479 --tx-format CS16 --rate 2e6 \
  --frequency 1e9 --offset 250e3 --tx-gain 70 --rx-gain 0 \
  --start-order tx-first --tx-duration 3 --rx-duration 2 \
  --output build/integration-evidence/corrected_switch_20261016/clean_forward_cs16_2msps.cs16
```

### Gain sweep and high-output limitation

The corrected path permits received-level measurements at manual RX gain 0.
For `8N55` TX → PCIe RX, Soapy TX settings 60 and 70 mapped to SDK
attenuation indices 119 and 79 and gave steady CS16 RMS levels 0.02245 and
0.07076 of full scale. Settings 75, 79, 79.75, and 80 (indices 59, 43, 40,
39) did **not** yield a clean higher-level tone: their captures had sparse
large peaks and crest factors 14.8, 18.1, 17.9, and about 19. The initial
FFT frequency/noise check had called these passes. The validator now rejects
a continuous-tone capture when RMS is at least 0.005 full scale and peak/RMS
exceeds 4. Saved high-gain captures fail this new assertion; clean setting-70
captures pass it. The reverse PCIe TX path was steady at settings 60, 70,
and 80. These are relative ADC levels, **not calibrated TX power or path
attenuation**.

The Python `tx_tone.py` repeats one 16,380-sample buffer. At 2 MS/s and
+250 kHz, this does cause a half-cycle phase jump at each buffer boundary.
However, the high-gain spike pattern persisted at +200 kHz, where the buffer
contains exactly 1,638 cycles and has no repeat-boundary phase jump. It also
appeared with SDK 4.26's own prebuilt `tx_tone` at attenuation index 40
(corresponding to Soapy gain 79.75), both when PCIe RX used the isolated
Soapy module and when the SDK's own `rx_to_file` captured the samples. The
SDK-only RX app used its default gain, so its amplitude is not directly
comparable, but its sparse-peak shape remained (crest about 24). The SDK TX
app was deliberately stopped by `timeout` (status 124); the finite RX apps
completed successfully. Evidence: `corrected_switch_sdk_tx_index40_compare_*`,
`corrected_switch_sdk_txrx_index40_compare_*`, their offline analysis logs,
and child logs/captures under the corrected-switch evidence directory.

This reproduces independently of the Python TX app and SoapySidekiq's TX and
RX stream implementations. It is an **unresolved RF hardware/setup or SDK
high-output behavior**, not an established SoapySidekiq defect. Use the
measured clean TX setting 70 for `8N55` on this path; do not treat the
setting-75-to-80 FFT-only results as clean-tone passes. A power meter or
spectrum analyzer and exact RF switch positions would be needed to isolate
analog compression, switching, or source behavior.

**Current assessment:** the selected `8N55`/PCIe functional matrix passes
with the corrected switches at the documented clean settings. Merge readiness
remains conditional on documenting the SDK initialization order and this
high-output RF limitation; the full TX gain range is not signed off. NV100
was outside that corrected-switch rerun; its later tests follow. No
production-driver code changed.

## 2026-10-16 NV100 two-port validation

The tested driver commit remained `4db75a965421a9ac79e2cce54fecf80bedfa79c8`
on `test/non-hardware-integration`. `nv100_module_identity` again found only
the candidate `libSidekiqSupport.so (4db75a9)` under
`build/integration-install`; SDK 4.26.0, SoapySDR 0.8.1, and GCC 11.4.0 are
unchanged. `nv100_probe` identified NV100 rev C, serial `9YAA`, Sidekiq card
2, FPGA 3.21.0, with 32767 CS16 host full scale. Soapy RX/TX channel 0 is
SDK A1/handle 0 on J1; channel 1 is A2/handle 1 on J2; channel 2 is B1/handle
2 on J2. The external GPS-aligned PPS feed was operator-reported; the tests
asserted pulses/timed lifecycle but did not measure UTC phase. The operator
confirmed that page 6 of `~/Downloads/RF config for NUC.pdf` represents the
installed reciprocal splitter arrangement between the mPCIe distribution
networks and both NV100 ports, and that X4 is disconnected. This physical
wiring statement is an **operator observation**, not a measured path-loss
assertion. No calibrated attenuation or RF power was measured.

All NV100 Soapy hardware runs used the isolated module, `tests/run_logged.sh`
(history in `logs/`, live `out.txt`), explicit 30–150 s outer timeouts, separate processes for
`9YAA` and `47479` in paired tests, and `9YAA` initialization before `47479`.
Captures and child logs are under
`build/integration-evidence/nv100_20261016/`. The nominal SDK profile was
**1.92 MS/s** with RX bandwidth 1.536 MHz; listed low/high profiles were
**1 and 10 MS/s**, each with RX bandwidth 80% of rate. A first 2 MS/s attempt
failed at `skiq_write_rx_sample_rate_and_bandwidth` with status `-71`; SDK
4.26's built-in profile table does not list 2 MS/s. It is a **test rate
selection error**, preserved as `nv100_rx_cs16_ch0_2msps`, and was replaced
with the valid 1.92 MS/s checks. A first finite-capture attempt failed because
the output directory did not yet exist; the setup was corrected and all
finite captures were rerun successfully.

| Check | Asserted result | Evidence labels |
| --- | --- | --- |
| RX counters/timestamps | CS16 RF and system timestamp continuity passed for A1/J1, A2/J2, B1/J2 at 1.92 MS/s (10,180 samples per mode); CF32 counter conversion passed on all three. CS16 A1/J1 and B1/J2 also passed at 1 and 10 MS/s. | `nv100_rx_cs16_ch*`, `nv100_rx_cf32_ch*` |
| RX lifecycle/partial/finite | A1/J1 and B1/J2 passed three 10-block activation cycles and 20 short reads in each timestamp mode. Both produced exact 10-block finite files: 40,720 bytes CS16 and 81,440 bytes CF32. | `nv100_loop_*`, `nv100_partial_*`, `nv100_finite_*` |
| mPCIe TX A1 → NV100 RX | At 1 GHz LO, 1.92 MS/s, mPCIe Soapy TX setting 60, NV100 manual RX gain 0, +256 kHz tone: CS16 A1/J1 measured +255.47 kHz, 54.2 dB tone/noise, RMS/FS 0.00038; A2/J2 measured +255.35 kHz, 65.1 dB, RMS/FS 0.00093; B1/J2 measured +255.47 kHz, 65.2 dB, RMS/FS 0.00093. CF32 TX also passed into A1 and B1. No clipping. B1/J2 CS16 passed at 1 MS/s (+249.39 kHz, 67.0 dB) and 10 MS/s (+499.27 kHz, 63.5 dB). | `nv100_rx_ch*_pcie_tx_gain60_*`, `nv100_rx_a2_j2_pcie_tx_gain60_cs16`, `nv100_rx_b1_pcie_tx_cs16_*` |
| NV100 TX A1/J1 → mPCIe RX A1 | At 1.92 MS/s, +256 kHz, manual mPCIe RX gain 0, NV100 TX setting 0: CS16/CF32 measured +256.52/+256.64 kHz, 51.4/51.7 dB tone/noise, RMS/FS about 0.00123, no clipping. Setting 20 raised CS16 RMS/FS to 0.01106, +256.64 kHz, 71.2 dB, crest 1.10, no clipping. At setting 20, 1 and 10 MS/s CS16 passed (+250.55/+500.49 kHz, 72.6/70.2 dB, no clipping). | `nv100_tx_a1_j1_*`, `nv100_tx_a1_pcie_rx_cs16_*` |
| Full duplex, supported cross-group pair | **PASS** with NV100 RX B1/J2 + TX A1/J1 and mPCIe RX/TX A1, 1.92 MS/s, 1 GHz LO, separate processes. NV100 TX setting 0, mPCIe TX setting 60, both RX gains 0. NV100 RX measured peer −320.27 kHz at 35.7 dB tone/noise, peak/FS 0.00119; mPCIe RX measured peer +256.29 kHz at 22.1 dB, peak/FS 0.00195; no clipping/stream error. | `nv100_full_duplex_rx_b1_tx_a1_1p92msps` |
| RX retune while TX active | **PASS** NV100 TX A1/J1 setting 0 and mPCIe RX A1 at 1.92 MS/s. Moving PCIe RX LO 1.0000 → 1.0001 GHz moved the tone +256.52 → +156.56 kHz; TX remained active, no clipping. | `nv100_tx_a1_pcie_rx_retune_1p92msps` |
| External PPS | **PASS** timed RX/TX starts/stops, pending-stream cleanup with zero host reads/writes, and immediate restarts at 1.92 MS/s, both RX A1/TX A1 and RX B1/TX A1 sequential lifecycle configurations. B1 timed RX start waited 0.862 s; timed stops waited about 1 s. GPS absolute phase was not independently asserted. | `nv100_pps_lifecycle_*` |

### Direct SDK C-app startup-order check

With SDK 4.26.0 prebuilt `tx_tone` on PCIe card 0 (`47479`, A1) and
`rx_to_file` on NV100 card 2 (`9YAA`, A1/J1), both startup orders passed at
1.92 MS/s, 1.536 MHz bandwidth, 1 GHz LO. PCIe TX used attenuation index
80 and a +256 kHz tone; NV100 RX used SDK counter mode, so this check asserts
initialization, concurrent streaming, and counter integrity, **not RF tone
reception**. Each app ran in its own process with an outer timeout and the
complete commands/results in `logs/`; child logs and captures are under
`build/integration-evidence/nv100_order_20261016/`.

* **PCIe TX first:** TX reached streaming before NV100 launch; NV100 then
  initialized, verified its counter capture, and exited 0 while TX was active.
  TX was intentionally stopped afterward (status 143). Evidence label:
  `nv100_sdk426_pcie_tx_first_retry`.
* **NV100 RX first:** RX reached streaming before PCIe launch; PCIe TX reached
  streaming while RX remained active; NV100 verified its 128 MB counter capture
  and exited 0. TX was intentionally stopped afterward (status 143). Evidence
  label: `nv100_sdk426_nv100_rx_first_overlap`.

The initial PCIe-first trial had an 8 s RX timeout during normal NV100
initialization; a standalone 35 s RX baseline passed, and the 35 s paired
retry passed. A first NV100-first trial used a 32 MB capture that completed
before PCIe TX reached streaming; the 128 MB rerun established overlap.
These were test-duration limitations, not SDK failures. **No startup-order
failure was observed for the NV100/PCIe pair in these two runs.** The separate
8N55/PCIe initialization crash above remains reproduced on SDK 4.24–4.26;
the NV100 result does not resolve that pair's issue.

A representative two-port full-duplex command was:

```sh
tests/run_logged.sh nv100_full_duplex_rx_b1_tx_a1_1p92msps \
  env PYTHONDONTWRITEBYTECODE=1 \
  SOAPY_SDR_ROOT=/home/dave/SoapySidekiq/build/integration-install \
  timeout 150s python3 tests/hardware/full_duplex_pair_validate.py \
  --dut-serial 9YAA --peer-serial 47479 \
  --dut-rx-channel 2 --dut-tx-channel 0 \
  --dut-rx-full-scale 32767 --peer-rx-full-scale 2047 \
  --rate 1920000 --frequency 1000000000 \
  --dut-offset 256000 --peer-offset -320000 \
  --dut-tx-gain 0 --peer-tx-gain 60 \
  --dut-rx-gain 0 --peer-rx-gain 0 \
  --dut-duration 12 --peer-duration 5 --startup-timeout 70 \
  --output-dir build/integration-evidence/nv100_20261016/full_duplex_b1_a1
```

### J2 transmit gap and RF-port-mode investigation

NV100 TX B1/J2 with Soapy settings 0 and 20 and TX A2/J2 at setting 20
reported successful TX writes and process exit, but the mPCIe RX capture
contained no +256 kHz tone. At B1 setting 20 the global spectral peak was
about +586 Hz and RMS/FS was 0.00053; changing from setting 0 did not raise
the level. These are **failed RF tone assertions**, preserved as
`nv100_tx_b1_j2_gain{0,20}_pcie_rx_cs16` and
`nv100_tx_a2_j2_gain20_pcie_rx_cs16`, with child logs and binary captures.
The mPCIe receiver and the J1 path were independently verified by the A1/J1
TX passes above.

SDK 4.26 documents `skiq_write_rf_port_config()` and
`skiq_write_rf_port_operation()` for changing supported TRX port operation.
The driver currently calls neither and only reads antenna mappings.
A direct SDK read probe, `nv100_rf_port_mode_read_fixed`, confirmed fixed mode
(`config=0`), both fixed and TRX available, A1→J1 and A2/B1→J2; operation
readback returned `-95` in fixed mode as expected. A bounded diagnostic used
the SDK functions from the same loaded candidate module before Soapy TX:
`skiq_write_rf_port_config(2, TRX)` succeeded and read back `config=2`, then
`skiq_write_rf_port_operation(2, true)` succeeded and read back transmitting.
**Neither B1/J2 nor A2/J2 produced the expected tone even with that explicit
TRX selection**; both TX/RX processes exited 0 and the RF assertion failed.
Raising B1/J2 to Soapy TX setting 40 under TRX mode still gave a +586 Hz
global peak and RMS/FS 0.00055, with no +256 kHz tone or meaningful level
increase. Evidence: `nv100_tx_{b1,a2}_j2_trx_sdk_mode_gain20_pcie_rx`,
`nv100_tx_b1_j2_trx_sdk_mode_gain40_pcie_rx`, diagnostic
script and captures in the NV100 evidence directory.

Thus the driver does lack RF-port-mode controls, but that omission is **not
proven to be the sole cause** of absent J2 RF output. SDK routing/topology,
hardware RF switching, splitter cabling, or a driver TX-path defect remain
possible. Do not sign off J2 transmit or the reverse full-duplex pair
(RX A1/J1 + TX B1/J2). Simultaneous multiple RX or TX handles on one NV100
are not supported by the current driver (`Streaming.cpp` limits one stream per
direction and one channel per stream), so multi-handle checks were capability
skips. No production-driver source was changed during validation.

**NV100 assessment:** the tested J1 TX / J2 RX configuration passes the
functional hardware matrix at the listed SDK profiles, including full duplex,
retuning, and PPS. Two-port TX coverage is incomplete because J2 TX has no
measured RF output. Merge readiness for a claim of both-port NV100 operation
is **blocked pending J2 path diagnosis and a separate review of any driver
fix**. The committed driver can be evaluated for the working J1 TX / J2 RX
capability with this limitation stated explicitly.
