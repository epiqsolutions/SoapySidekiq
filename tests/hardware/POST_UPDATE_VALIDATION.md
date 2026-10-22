# Hardware follow-up for the update series

Status: procedure, not a result record. The [2026-10-22 hardware results](../../docs/validation/2026-10-22-hardware-results.md)
record 58 matrix commands on the driver through #145 and distinguish remaining
follow-up gaps. The
[original campaign](INTEGRATION_TEST_PLAN.md) used its pinned historical
candidate. This follow-up covers later ownership, configuration, buffering, and
PPS changes after their reviews and integration are complete.

## Pin the candidate and evidence

Use one exact integrated commit throughout the run. Save the commit, SDK
version/build configuration, SoapySDR API/ABI, compiler, module install prefix,
card serial/model/revision, topology, logical-channel/SDK-handle map, and PPS/RF
connections. Save commands, return codes, logs, sample counts, timeouts, and
measured results beside the review/release record. Repeat affected cases after
any code or configuration change.

An isolated `SoapySDRUtil --check=sidekiq` confirms module discovery without
opening a card. `--find`/`--probe` need cards, and sample-path tests need supported
RF routing. The existing examples and full-duplex validators remain the starting
points; use their `--help` for the current finite-run options.

## Checks without an external RF source or analyzer

1. Run normal and ASan/UBSan suites and the native SDK module build/install/load
   checks on the pinned tree. Run TSan on a host whose runtime starts; retain an
   explicit environment failure rather than counting it as a pass. Repeat the
   module checks on supported ARM hardware or label a cross-build as such.
2. With TX inactive, check discovery, independent device construction, channel
   metadata, sensors, supported rate/profile readback, invalid requests, and
   repeated open/close. Record card-specific unsupported operations. The
   noninteractive [API diagnostic](test_api.py) performs capability/readback checks;
   inspect its help and output rather than assuming every card has every sensor.
   Construction performs normal SDK initialization; an explicit topology changes
   construction settings, and `--read-sensors` opts into sensor sampling.
3. After shared SDK ownership is integrated, keep one card receiving while
   independently opening/closing a second card in the same process. Verify
   closing one does not interrupt the other. Exercise an absent/busy second-card
   request and duplicate independent construction. Initialize M.2 before PCIe
   TX for the known pair. Separate processes do not establish shared lifetime.

## Checks using a known RF path

Use the original campaign's documented connected ports, accepted rates, and
operating levels. A paired Sidekiq transmitter/receiver can supply and measure
the relative tone: an external signal generator/analyzer is optional for these
functional checks. Absolute power, calibrated sensitivity, and spectral
qualification need suitable calibrated equipment and separate acceptance limits.

1. Repeat finite CS16/CF32 RX, one-way tone, and paired full-duplex checks on the
   supported card/profile paths. Verify received samples, expected tone offsets,
   clipping, timestamp continuity, accepted TX counts, and reported overruns or
   underruns. Preserve unsupported ports/profiles as explicit limitations.
2. Exercise configured TX counts such as 64 and 1024 at representative low/high
   accepted rates. Record block size, pool count, process peak RSS, throughput,
   underruns, write latency/timeouts, and accepted prefixes. Include the default
   pool only when the node has enough memory for its documented allocation.
   Warm up, measure for a stated duration, and repeat; host allocation timings
   alone cannot justify changing the compatibility default.
3. Repeat activate/write/deactivate/close and device destruction with accepted
   asynchronous TX sends. Check stop errors/retries, sustained RSS across cycles,
   late completion behavior, and whether SDK callbacks finish before card release.
   Preserve SDK logs and any failure. A stable RSS observation is not a proof of
   a callback barrier or release of every context.
4. Keep TX active while stopping and retuning RX between two supported settings,
   then restarting RX. Verify TX submissions continue and RX returns the expected
   tone/rate/timestamp interpretation. Rate/bandwidth and clock/time changes
   that affect RX must be rejected until RX deactivation succeeds; test the
   accepted inactive transaction and next activation separately.

## PPS and pending startup

Confirm the selected source has observable edges before timing checks. Record
source selection, edge observation, start/stop mode, and whether RX and TX use
the same card. Exercise immediate and next-PPS starts independently, then the
supported combined RX/TX sequence. Verify activity begins at the expected edge
and the first write observes startup success/failure correctly.

While TX startup is waiting for an edge, check finite write timeout, writer
cancellation during deactivation, rejection of active close, and restart after
startup has returned and stop has completed. Supply an edge to allow the normal
join/stop path to finish. The production-method tests cover this controlled
ordering; hardware must establish the SDK/card behavior.

The [SDK lifetime review](../../docs/SDK_LIFETIME.md) distinguishes the SDK's
documented missing-PPS timeout history from caller-controlled cancellation.
No bounded cancellation path is established in this series. For an intentionally
absent-edge diagnostic, use a separate bounded process and record elapsed time,
the SDK return status if it returns, and whether the outer process timeout fired.
Keep SDK startup errors separate from writer deadlines. Do not mark teardown
as passing because the outer process was killed, or require destruction to
return before delivering an edge. A process timeout can leave recovery work
for the operator/SDK; record that outcome before reusing the card.

## Completion record

List each check as pass, failure, capability skip, or setup skip with its exact
command and evidence location. Attach the summary to the reviewed candidate or
release record. Record remaining SDK callback/PPS limits and untested hardware
separately. The earlier campaign's completion does not complete this follow-up.
