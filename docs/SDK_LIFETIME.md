# SDK lifetime and PPS evidence

Reviewed on 2026-10-21 against the installed Sidekiq SDK 4.26.0 header and
software manual. Paths below are relative to an SDK installation; these
licensed documents are not copied into this repository. This is a source
documentation review, not a hardware result or an SDK implementation audit.

## What the reviewed documents establish

| Source | Documented behavior | Driver implication |
| --- | --- | --- |
| `sidekiq_core/inc/sidekiq_api.h`, `skiq_write_tx_transfer_mode`; `doc/sdk_manual/html/developing_apps.html`, “Writing I/Q Samples to a Tx Interface” | An asynchronous successful send can mean queue acceptance before FPGA transfer. Completion identifies when the application can reuse/free that buffer; before completion the library still uses it. | Accepted `TxSendContext` objects retain both the block and isolated completion bookkeeping. A returned accepted prefix does not mean its blocks can be freed immediately. |
| API header, `skiq_register_tx_complete_callback` | Completion reports processing finished for its data pointer. | The callback can release the corresponding retained block/context. The reviewed entry does not specify a drain barrier for callbacks already in flight when registration changes. |
| API header, `skiq_stop_tx_streaming`; manual, “Stopping and Releasing an Interface” | Stop disables the CPU/FPGA streaming interface until another start. | Stop is part of shutdown; the reviewed text does not explicitly specify callback draining or disposition of every accepted user context on errors. |
| API header, `skiq_exit` and `skiq_disable_cards`; manual, “Exiting” | Exit shuts down the library for the application; disabling selected cards does not replace final library exit. SDK calls from a signal handler can deadlock on library mutexes. | Coordinate process-wide/card ownership, as proposed in #85. Perform orderly stop/join/SDK cleanup in ordinary application execution, outside signal handlers. |
| API header, `skiq_start_tx_streaming_on_1pps`; manual, “Starting and Stopping on 1PPS” | PPS start waits for the selected edge, and timestamp zero requests the next edge. | The driver worker owns this blocking operation; a writer deadline does not cancel it. |
| `doc/sdk_manual/html/release_history.html`, v4.9.1 and v4.9.4 entries | The history records a missing-PPS timeout and a later correction accounting for the requested system timestamp. | Do not describe SDK PPS startup as necessarily waiting forever. These entries do not establish a tested universal return bound or a caller-controlled cancellation operation for every supported 4.26 card/transport. |

The supplied example
`example_apps/test_apps/src/tx_samples_async.c` counts completed sends and
normally waits for that count to catch up before cleanup. Its interrupted/error
paths and cleanup order are example behavior, not an explicit API guarantee
that every pending callback is drained by stop or exit. They cannot justify
freeing an accepted context that the SDK might still access.

## Software guarantees and remaining questions

The driver closes API admission, wakes/drains admitted callers, joins the PPS
worker, and stops the stream before releasing device owners. Delivered TX
callbacks use retained state without accessing the device. The controlled
worker/destructor and late-callback tests establish that host ordering for
scripted returns and delivered callbacks. They do not prove the SDK's return
timing, cancellation, or callback delivery behavior.

Keep the unconditional release/quiescence plan items open until there is an
explicit applicable SDK contract or other sufficient evidence for:

- Whether successful and failed stop/disable/exit paths drain all in-flight
  callbacks, and when that barrier has completed.
- Whether every accepted send receives a callback on those paths; if not,
  which operation establishes that its block and user context are no longer
  accessible and may be reclaimed.
- Missing-PPS return status and elapsed time for each supported card/transport,
  and whether any SDK mechanism can cancel a pending start independently of
  its internal timeout or an arriving edge.

The [hardware follow-up](../tests/hardware/POST_UPDATE_VALIDATION.md) records
observable outcomes. A finite test without a late callback is not proof of a
quiescence barrier; killing a timed-out process is not successful cleanup.

## Clarification request for Epiq

The following questions are ready to send with the SDK version, card/transport,
and hardware run evidence. No vendor request has been sent by this repository
update. Please ask for a documented contract, including error returns and any
version/card/transport restrictions, rather than inferring a barrier from one
successful run.

1. After `skiq_stop_tx_streaming` or `skiq_stop_tx_streaming_on_1pps` returns,
   can the SDK still read a previously accepted application's TX block or invoke
   its completion callback? Distinguish success, already-stopped, and other errors.
2. Does `skiq_register_tx_complete_callback(card, nullptr)` wait for callbacks
   already executing or queued? Can a captured callback still run afterward?
3. After successful `skiq_exit`, are all reads of application TX blocks and
   uses of their `p_user` contexts finished? If it is a barrier, does it also
   guarantee callback delivery for every accepted send? If sends can be dropped
   without a callback, what operation establishes safe reclamation of those
   blocks/contexts? What is safe after a failed exit?
4. If stop/unregister/exit are not the required barrier, which supported API
   sequence establishes it? The driver must distinguish safe block reuse from
   callback delivery and from completion of callbacks already running.
5. For SDK 4.26 and supported successors, what controls the return deadline and
   status of `skiq_start_tx_streaming_on_1pps` when the selected PPS source is
   absent? The release history mentions timeout handling; state the applicable
   bound and conditions. Is there a supported way for another thread to cancel
   a pending start, and is calling stop during that pending start valid?

Current host policy is conservative: successful submission transfers context
ownership to completion; accepted contexts retain their blocks and bookkeeping;
device cleanup joins the startup worker after the SDK call returns. Scripts test
late delivery and controlled startup release, but do not prove answers to these
questions. Until the applicable contract is established, do not reclaim a
possibly SDK-visible accepted block merely because a driver timeout expired.
