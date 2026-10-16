# Hardware integration test plan

Candidate: `test/non-hardware-integration` at
`4db75a965421a9ac79e2cce54fecf80bedfa79c8`.

This is the run order and evidence checklist for the combined candidate. Record
results in [INTEGRATION_VALIDATION.md](INTEGRATION_VALIDATION.md). Stop at the
first unexplained failure, save the command and output, determine whether the
cause is the driver, test, unsupported capability, or setup, then rerun the
affected check after any fix. Put production fixes and regression tests on a
separate review branch.

## Node and RF setup

The node has Sidekiq SDK 4.26.0, SoapySDR 0.8.1, GCC 11.4.0, and these cards:

| Card | Serial | Soapy card | Channels reported by probe | Host CS16 full scale |
| --- | --- | ---: | --- | ---: |
| PCIe MPCIE C | `47479` | 0 | RX 0, TX 0 | 2047 |
| M.2-2280 rev B | `8N55` | 1 | RX 0, TX 0 | 2047 |
| NV100 rev C | `9YAA` | 2 | RX/TX 0, 1, 2 | 32767 |

An operator reports a GPS-aligned external PPS box feeding all three cards.
The reported wiring is `8N55 TX -> 47479 RX`, `9YAA TX -> 47479 RX`,
`47479 TX -> 8N55 RX`, and `47479 TX -> 9YAA RX`, through splitters with no
additional attenuators. Do not transmit from `8N55` and `9YAA` into the common
PCIe RX simultaneously. The operator confirmed page 6 of
`~/Downloads/RF config for NUC.pdf`: added reciprocal splitter paths connect
both NV100 J1 and J2 to the receive and transmit distribution networks. X4 is
disconnected. The probe maps NV100 channel 0/A1 to J1 and channels 1/A2 and
2/B1 to J2 by default; channels 1/2 share J2 and may require a topology
choice. Select the actual port and handle pair before each test.

NV100 uses discrete SDK rate/bandwidth profiles. The valid low, nominal, and
high rates used here are 1, 1.92, and 10 MS/s, with receive bandwidth 80% of
the rate. The 2 MS/s request is not a built-in SDK profile and was rejected.
NV100 starts in fixed RF-port mode; the SDK supports TRX mode through
`skiq_write_rf_port_config()` and `skiq_write_rf_port_operation()`, but the
current driver does not expose those calls. The J2 TX A2/B1 tone path remains
unresolved even after a diagnostic explicitly selected SDK TRX mode, so do not
claim that the missing call alone explains the absent tone.

The driver maps Soapy TX `setGain(..., 0)` to the **maximum SDK attenuation
index**, hence the lowest output level. A larger Soapy TX value *reduces*
hardware attenuation. The valid Soapy TX ranges are 0–89.75 for PCIe/M.2 and
0–41.75 for NV100. Start at 0, verify the logged SDK index and readback, and
increase only as needed. Start RX in manual gain mode at 0 dB. Check the
captured peak and clipping fraction after each change. Keep the signal below
0.8 of ADC full scale with no sustained clipping; if the tone is too weak,
raise the TX setting or RX gain in small steps and record both. For a
continuous tone with RMS at least 0.005 of full scale, reject a peak/RMS crest
ratio over 4; a sparse spike can otherwise masquerade as a strong tone. The splitter
loss and RF power have not been measured; do not claim a calibrated absolute
power result from these captures.

## Common execution rules

Use the isolated module installed at `build/integration-install`, never the
system module. Set `SOAPY_SDR_ROOT` to that absolute prefix for every Soapy
command. `SoapySDRUtil --info` must show only the module at that prefix with
version `4db75a9` before each test group. Save stdout/stderr, exit status,
module path, SDK/Soapy versions, card model and serial, Soapy and SDK handles,
topology, rate readback, format, frequency, RF/PPS path, TX setting and RX gain.
Keep binary captures under `build/integration-evidence`.

Run each test through `tests/run_logged.sh LABEL COMMAND [ARGS...]`. It keeps
timestamped local history in `logs/` and writes a live copy to `logs/current.log`;
`out.txt` is a stable symlink to that file for `tail -F out.txt`. Keep large
binary captures under `build/` and record
their paths in the validation report.

Use `timeout` around every executable: 30 seconds for probes and counter or
lifecycle runs, and 15 seconds for each finite tone/capture stage unless the
measured initialization time requires a documented increase. Use 200,000 us
per stream read, small block counts, and 1–3 second RF captures. Run hardware
steps serially except when two processes are explicitly needed for a paired
full-duplex check. A process timeout, zero samples, SDK error, or missing
timestamp is a failed assertion, not a pass. Distinguish numeric assertions
from visual or operator observations in the record.

## Walk through

1. **Identity and build.** Verify the exact Git commit, SDK version via
   `sidekiq-config --version`, `SoapySDRUtil --info`, and compiler version.
   Configure/build the actual driver against `/home/dave/sidekiq_sdk_v4.26.0`,
   install under `build/integration-install`, and verify the isolated module
   with `--check=sidekiq`. Run the normal and ASan/UBSan non-hardware suites
   with separate output logs. The current candidate has built/linked and both
   suites passed 109/109 cases on this node.
2. **Discovery and topology.** Run bounded isolated `--find` and `--probe` for
   each serial. Assert serial-to-card mapping, channel counts, handle/port
   mapping, formats, full scales, and supported rates. PCIe/M.2 are single
   channel. Inspect available NV100 topologies before any multi-handle stream;
   do not infer simultaneous behavior from `getFullDuplex` alone. Mark Z4
   topology as a capability skip because no Z4 is installed.
3. **RF-independent RX.** For each selected RX channel, run
   `cs16_validate.py`, `cf32_validate.py`, and `run_loop.py` with serial and
   channel specified, 10 blocks or fewer, and an outer timeout. Use 1, 2,
   and 10 MS/s as proposed low/nominal/high rates only where set/readback
   confirms support; otherwise record the accepted rate or a capability skip.
   Check both RF and system timestamp continuity. Add a bounded partial-read
   check that requests less than the MTU, verifies sample order and timestamp
   advancement, and checks finite CS16/CF32 capture file sizes.
4. **One-way RF.** At a nominal 1 GHz LO and 2 MS/s, test each confirmed path
   one at a time. Transmit a finite tone at +250 kHz in both CS16 and CF32.
   Capture on the paired RX and measure the FFT peak frequency, tone-to-noise
   ratio, peak amplitude, and clipping fraction. Use frequency tolerance of
   the larger of two FFT bins or 20 kHz to allow independent references.
   Repeat at accepted low/high rates where practical. Record settings and
   actual level changes, not just that the example exited successfully.
5. **Paired full duplex.** On the PCIe/M.2 pair, run RX and TX on both cards
   concurrently with bounded 6-second DUT and 4-second peer processes, with the two physical directions
   connected as stated. Assert both receivers obtain samples and both tones
   appear at their expected offsets without clipping or stream errors. Repeat
   on NV100 only after confirming its physical ports and supported topology.
6. **RX retuning while TX stays active.** Use a supported paired channel/RF
   configuration. Keep the source TX stream active while the paired RX stream
   stops, retunes between two nearby LO settings, and restarts. Verify both
   captures contain the same transmitted tone at the expected relative
   frequencies and that TX sample submission continues across the retune.
   A same-card `rx_retune_while_tx.py` run is appropriate only when its TX and
   RX ports are actually connected through the stated lab path.
7. **PPS.** The selected cards have an external GPS-aligned PPS box. Test
   external timed RX/TX starts/stops; test internal PPS only if that source is
   selected for a separate run. Confirm
   that no samples or TX writes occur before the edge, that activity begins
   after it, and that immediate close/deactivation cleans up a pending start.
   Bound the wait to a few edges. Record absent sources and unsupported
   combinations as setup or capability skips, never as passes.

For any failure, preserve the failing log and a minimal repeat command. Check
rate readback, gain/attenuation index, RF port, topology, and module version
before assigning a driver defect. Do not merge into master as part of this
validation.
