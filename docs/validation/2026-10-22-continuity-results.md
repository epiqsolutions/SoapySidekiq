# Same-card continuity observation, 2026-10-22

Result: **FAIL at the configured 100 microsecond / 12 dB envelope criterion**.
This is a new observation beyond the earlier 58-command matrix; that matrix's
passes do not override this result.

The software session ran the test via SSH on
`dave@192.168.2.151:/home/dave/SoapySidekiq`, using the existing isolated
production module `7d295b4551004ea411fab550017e9b93d6cac6d8` with SDK 4.26.0,
SoapySDR 0.8.1 / ABI `0.8-3`. DUT M.2 `8N55` transmitted to PCIe `47479`
through the operator's existing RF path. Settings were 2 MS/s, 1 GHz LO,
+250 kHz requested tone, TX attenuation 70 dB, observer gain 0, four RX
stop/100 kHz LO-retune/restart cycles, 0.5-second dwells, and 1024 TX buffers.
The coherent MTU tone's actual offset was +250061.05006105007 Hz.

## Test identity and startup correction

The first script (`6b8ea63`) initialized PCIe before M.2 and lost its DUT worker
during M.2 initialization, before recording any measurements. This order was
inconsistent with the known working M.2-first order for the pair. The parent's
EOF did not establish the child's precise cause or exit status. That run is
retained as a failed test setup, not a continuity result.

The corrected script at `c35384c` initializes the DUT first, waits for observer
initialization before TX startup, and reports child EOF with its exit code.
It completed the four retune cycles and normal worker/observer cleanup. Neither
the production driver nor its installed module was changed.

The node could not fetch GitHub using its SSH credentials. The committed script
was copied into `build/continuity-test-c35384c/tx_continuity_validate.py` with
matching local/remote SHA-256
`eaabefb27fb56f6ca4da3bbd8d25f572975ebe8c2946f26487f93ab670c96a95`.
The node's branch stayed `fix/hardware-api-unconfigured-lo`, HEAD `e52796e`;
its logged HEAD is distinct from both the script and module identities.

## Measurements

The observer saved 7,044,560 complex samples (3.52228 seconds), with continuous
per-read RF timestamps under the test's one-sample rounding tolerance. The
initial 0.2-second median envelope was 36.7151 CS16 units (full scale 2047).
Offline examination of the retained capture with the same 12 dB relative
threshold found these four longest low-envelope intervals. Sample indices are
zero-based; the end index is exclusive.

| RX cycle | Sample start | Sample end | Duration |
| --- | ---: | ---: | ---: |
| 0 | 1007435 | 1008492 | 528.5 us |
| 1 | 2015124 | 2016089 | 482.5 us |
| 2 | 3021500 | 3022523 | 511.5 us |
| 3 | 4029257 | 4030275 | 509.0 us |

The intervals follow the respective cycle-begin events. TX accepted counts
increased in every cycle, and RX frequency readbacks matched 1.0001/1.0000 GHz.
This separates ongoing host submissions and observer capture from the observed
RF envelope interruptions; it does not identify the cause of those interruptions.
The driver RX frequency setter forwards to `skiq_write_rx_LO_freq` and does not
stop TX itself. Direct SDK/control comparisons would be needed before attributing
the gaps to the driver or treating them as a documented card/SDK limitation.
No thresholds were relaxed to turn this observation into a pass.

## Retained evidence

All paths below are relative to the node's repository. Raw evidence remains
Git-ignored and node-local. `out.txt` follows `logs/current.log`; complete
per-run logs are retained independently.

| File | SHA-256 |
| --- | --- |
| `logs/20261022T164230Z_same_card_continuity_6b8ea63_SagfYd.log` | `a8bdd3a32477886bade19e7936818203e3525efed63cc6168dbc22060af58776` |
| `logs/20261022T164417Z_same_card_continuity_c35384c_n3v08B.log` | `02a5c1f4420e7475398df14169452ef7a61aa49ce134cd9e664fb37783435452` |
| `build/integration-evidence/master_20261022/same-card-continuity-c35384c-001/result.json` | `56a859db57b40970eba54a6d4e09365bbec0bd6f6467e59cf1b90285d28d14b2` |
| `build/integration-evidence/master_20261022/same-card-continuity-c35384c-001/observer.cs16` | `103c1be4538f38c78eb9748098a78f626920f75b2052dd95127a116468989fb7` |
| `build/integration-evidence/master_20261022/same-card-continuity-c35384c-001/blocks.jsonl` | `908b9e2960cd645d664ddd2fd7592438e8cf2ee7928dc2b86960d5bd5511401e` |

The command and exit status 1 are retained in the corrected run log. The
[procedure](../../tests/hardware/POST_UPDATE_VALIDATION.md#same-card-tx-envelope-continuity-during-rx-retuning)
describes the measurement limits. This observation covers one card pair,
immediate starts, and LO changes, not PPS combinations, rate/bandwidth changes,
other cards, phase/sample identity, or SDK callback quiescence.
