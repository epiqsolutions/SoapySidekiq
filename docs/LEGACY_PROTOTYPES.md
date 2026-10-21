# Legacy prototype decisions

The channel-mapping and TX evaluations use production baseline `92660c9` and the retained
`test/non-hardware-suite` snapshot
`53a59443ec8c5131df3f8c9e8dbb15234f0522c3`. It records which ideas remain useful
before further channel-mapping and TX ownership changes. It does not establish
that the remaining TX shutdown paths are safe.

## Channel mapping

The prototype's `include/SoapySidekiq/ChannelMap.hpp` stores separate RX/TX
vectors, checks a logical channel against the matching vector, and returns its
handle. Its counts are vector lengths. It does not select a topology or manage
SDK ownership.

The production equivalent is already implemented through `validateChannel()`,
`getRxHandle()`, `getTxHandle()`, and `getNumChannels()`. Constructor parsing
validates card/topology options before configuration. After topology selection,
the constructor reads the SDK parameter table and channel counts; mapping
methods use the selected direction's actual table entry. Stream setup validates
its channel list through `validateStreamRequest()` before looking up the handle.
Tests exercise reordered/noncontiguous handles, count boundaries, oversized
indices, missing directions, and stream metadata.

Decision: retain the production mapping boundary. A second vector cache would
represent the same SDK-derived mapping and add another state to synchronize.
The old template contributes no missing mapping behavior needed by the current
single-channel stream contract. Its boundary and noncontiguous-handle cases
remain useful regression scenarios and are already represented in the tests.
Reconsider a separate mapping object if future live topology changes or
multi-channel streams introduce a different ownership/configuration model.

## Asynchronous TX pool and writer

The same prototype contains `AsyncBufferPool.hpp`, `TxStreamWriter.hpp`,
`TxStreamWriter.cpp`, and `SidekiqTxBackend.cpp`. The retained
`bugfix/tx-buffer-ownership` snapshot
`166a0e545e1131995eb1aa8020848d0b3b41b6b0` also contains an earlier production
callback/context implementation. The production path has since gained its own
reservation guards, deadline waits, cancellation, and callback exception tests.

Useful ideas to carry forward:

- A move-only reservation releases a rejected or locally failed submission.
- The writer bounds in-flight work and retains callback completion state through
  a shared owner. This separates completion bookkeeping from a writer object's
  lifetime.
- Back-pressure and completion notification are tested independently from RF
  hardware, including completion order and writer cancellation.

These implementations are not ready to adopt unchanged:

- `AsyncBufferPool` owns slot states. It does not own SDK TX blocks, accepted
  callback contexts, or SDK/card lifetime. Its lease contains a raw pool pointer,
  so the caller must still prove that the pool outlives outstanding leases.
- The prototype writer stages partial hardware blocks. Production currently
  requires whole-MTU writes and reports accepted prefixes; adopting staging
  would change the public buffering, timeout, and stop behavior.
- The SDK adapter allocates a block before allocating its callback context;
  a context-allocation exception can bypass block cleanup. Its callback also
  invokes a potentially throwing completion before freeing the block, without
  containing exceptions at the C boundary.
- The prototype writer destructor ignores a failed one-second completion drain.
  Adapter shutdown unregisters a callback without demonstrating that already
  admitted callbacks have finished. Neither step supplies the missing lifetime
  guarantee needed to free callback-visible state.

Decision: reuse the reservation/shared-completion-state concepts and their
regression scenarios when designing production ownership. Keep the existing
whole-MTU contract and one deadline per write. Introduce explicit SDK block
ownership together with a close/destruction policy that accounts for accepted
submissions and callbacks; replacing raw members with automatic owners alone
would release memory without resolving the existing callback lifetime problem.

## Requirements for the next ownership change

At the evaluated production baseline, accepted contexts still refer to a device
through `classAddr`, and SDK TX blocks are raw pointers. A callback can therefore
need device/stream state after submission has returned. Writer/status-call
draining does not by itself drain completions.

The SDK 4.26 declaration documentation for `skiq_register_tx_complete_callback()`
in `sidekiq_core/inc/sidekiq_api.h` says that the submitted memory has completed
processing when the callback is called. The stop and callback-registration
interfaces do not state a callback-quiescence barrier in that header. This
review does not infer such a barrier from a successful return code.

The production change must provide and test:

1. One owner for each SDK block with the SDK's matching deleter. Partial pool
   allocation and rejected submissions must release their acquired resources.
2. Completion state that remains valid for every accepted submission, including
   synchronous and out-of-order callbacks. Closing and reopening a stream must
   prevent an old completion from changing the new stream's slots or counters.
3. A shutdown order covering admission closure, admitted API calls, asynchronous
   PPS startup, hardware stop, outstanding callbacks, and eventual SDK/card
   release. Failed stops and missing completions need an explicit recovery or
   retention policy before callback-visible storage can be destroyed.
4. Regressions for callback/close races, failed-stop retries, allocation failures,
   and destruction with outstanding work. A configurable pool limit must retain
   the same acceptance, timeout, and cancellation guarantees.

The separate shared-SDK lifetime proposal in PR #85 covers library/card owners;
it does not establish these stream-buffer and callback guarantees. Its review
and hardware evidence remain separate from this prototype evaluation.

## RF configuration backend

This follow-up compares production `6f7665b` with the retained RF prototype at
`5ec9c7c` (`RfConfiguration`, `RfBackend`, `SidekiqRfBackend`, and their fake-backend
tests). The prototype isolates SDK operations behind virtual methods and owns
channel maps, per-direction RF snapshots, and gain-profile conversion.

Its mapped-handle, conversion, and backend-failure scenarios are useful and are
represented by the production-method frequency/gain/rate/bandwidth fixtures.
Those fixtures compile the actual RF methods against scripted SDK functions;
a second production RF interface is not required to inject these failures.
The real RX session ownership boundary is tested separately with real session
objects, rather than assuming the RF fixture's scripted gate covers concurrency.

Do not adopt the prototype's behavior unchanged:

- Its bandwidth setter preserves a cached rate. A coupled-clock change in
  another direction can make that snapshot stale; production reads the current
  configured rate under the RX lifecycle gate before the write.
- Its sample-rate getter returns the configured integer rate, and its SDK
  frequency adapter returns the requested integer frequency. Production getters
  retain the actual backend values, including fractional readbacks.
- Its independent state vectors and channel map would introduce another copy
  of state already owned by the device. It has no gate serializing these RF
  transactions with production RX activation and failed-stop ownership.

Decision: retain the existing production RF boundary and reuse the prototype's
regression ideas. The coupled-failure cases additionally model a write changing
hardware before reporting failure, and failed/invalid/missing post-write
readbacks. Fresh getters and opposite-direction retries observe backend state;
no host-state policy can imply hardware rollback or establish physical coupling.
The subsequent dead-state audit removed the four retained RF snapshots because
production no longer consumed them. Constructor and post-write validation are
preserved, and the regressions exercise their errors and observable readback
rather than snapshots with no consumer.

Reconsider a distinct RF backend if another hardware implementation or a stable
end-to-end injection requirement provides a concrete use. Shared SDK/card
integration and hardware cross-direction validation remain separate work.

## Full fake-device prototype

The `a593d96` prototype's `FakeSidekiqDevice.hpp` and seven accompanying cases
test a separate simulator: card lookup, its own initialized flag, queued RX/TX
events, and stored callbacks. They do not construct the production device or
execute its SDK backend. Its shutdown clears pending callbacks, which cannot
establish the current retained-context or SDK quiescence contract.

Decision: retain the scenario ideas and use the
[native scripted-SDK integration target](../tests/sdk_integration/README.md)
for the small combined path. It compiles the actual concrete driver, registry
adapter, and backend with installed SDK types, and links only scripted or
fail-fast SDK definitions. Three cases connect discovery, construction,
configuration, both stream formats/directions, delivered callbacks, constructor
rollback, and shutdown. The existing portable fixtures cover the detailed
matrices. No additional production backend interface or copied simulator is
needed for this boundary; physical SDK guarantees remain unverified.
