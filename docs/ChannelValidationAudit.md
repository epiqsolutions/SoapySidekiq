# Channel validation and mapping audit

Audited on 2026-10-21 against master `3b17726`. This records the implemented
SoapySidekiq entry points and their existing regression coverage. It is not a
hardware result or a claim that unmerged API additions have been audited.

Follow-up against master `694e694` plus the RF snapshot cleanup: the same 28
scalar-channel methods remain. Unused retained rate/bandwidth vectors have been
removed; constructor validation and fresh SDK readback still use every mapped
handle. The RF/session regressions now also exercise the real ownership gate
and fresh-rate restart behavior. Pending API contributions remain outside this
inventory.

## Common boundary

`validateChannel(direction, channel)` asks `getNumChannels(direction)` to reject
unknown directions, then rejects `channel >= count`, including an absent
direction and oversized `size_t` values. `getRxHandle` and `getTxHandle` validate
before indexing the corresponding SDK parameter table. The logical channel is
the table position; its SDK handle can be non-contiguous or out of order.
Constructor validation bounds both counts by the SDK table capacities before
traversing them.

[DeviceLifecycle.cpp](../DeviceLifecycle.cpp) implements these checks.
[ChannelMetadataTests.cpp](../tests/non_hardware/component/ChannelMetadataTests.cpp)
uses RX handles 3/7 and TX handles 6/2/9 and checks invalid directions, count
boundaries, oversized indices, and empty directions.
[ConstructorChannelCountTests.cpp](../tests/non_hardware/component/ConstructorChannelCountTests.cpp)
also covers constructor channel-count limits.

## Implemented channel-taking entry points

The table accounts for all 28 scalar-channel methods declared by the concrete
device at the audited revision, counting each named/unnamed overload separately.
The sample-rate-range compatibility declaration counts once. Delegates validate
before their first SDK access or channel-indexed state access. Implementations
are in [RFControls.cpp](../RFControls.cpp) and [Streaming.cpp](../Streaming.cpp).

| Entry points | Validation and selected-channel use | Existing production-method fixture |
| --- | --- | --- |
| `getStreamFormats`, `getNativeStreamFormat`, `getStreamArgsInfo` | Direct validation; native full scale follows direction. Metadata performs no handle-specific hardware access. | [ChannelMetadataTests.cpp](../tests/non_hardware/component/ChannelMetadataTests.cpp) |
| `listAntennas`, `getAntenna`, `setAntenna` | `currentAntenna` validates and reads the supplied channel's mapped RX/TX handle. Selection accepts only its current reported port. | [AntennaTests.cpp](../tests/non_hardware/component/AntennaTests.cpp) |
| `hasDCOffsetMode`, `setDCOffsetMode`, `getDCOffsetMode` | Direct validation, including manual-only TX; RX calibration uses the supplied mapped handle. | [DcOffsetTests.cpp](../tests/non_hardware/component/DcOffsetTests.cpp) |
| `listGains`, `hasGainMode`, `setGainMode`, `getGainMode` | Direct validation; RX mode operations use the supplied mapped handle. TX reports manual mode without an SDK call. | [GainModeTests.cpp](../tests/non_hardware/component/GainModeTests.cpp), [GainTests.cpp](../tests/non_hardware/component/GainTests.cpp) |
| Named and unnamed `setGain`, `getGain`, `getGainRange` | Unnamed methods validate directly; named methods validate through `validateGainName` and `listGains` before delegating. Reads/writes use mapped handles; TX limits use the selected parameter-table entry. RX advertised range still follows the card profile at this revision; see pending #80 below. | [GainTests.cpp](../tests/non_hardware/component/GainTests.cpp) |
| `setFrequency`, `getFrequency`, `getFrequencyRange` | Direct validation; reads/writes use the supplied mapped handle. The SDK range query is card-wide per direction, after channel validation. | [FrequencyTests.cpp](../tests/non_hardware/component/FrequencyTests.cpp) |
| `setSampleRate`, `getSampleRate`, `setBandwidth`, `getBandwidth` | Setters validate before their lifecycle/configuration transaction; getters delegate to validated `readRateAndBandwidth`. SDK access uses the supplied channel. Setters reject changes while RX owns hardware or a worker. | [RateBandwidthTests.cpp](../tests/non_hardware/component/RateBandwidthTests.cpp), [RxLifecycleTests.cpp](../tests/non_hardware/component/RxLifecycleTests.cpp) |
| `getSampleRateRange`, `listSampleRates` | Range validates directly; listing delegates to range. SDK min/max are card-wide, so validation does not imply per-handle rate capabilities. | [SampleRateCapabilityTests.cpp](../tests/non_hardware/component/SampleRateCapabilityTests.cpp) |

## Stream channel lists and opaque handles

`setupStream` translates the Soapy direction before choosing RX/TX state, then
calls `validateStreamRequest`. An empty list selects logical channel zero;
multiple channels, unavailable channels, unsupported formats, and duplicate
streams are rejected. Setup captures the selected mapped handle in the owned
stream token. Subsequent stream operations use that token rather than a mutable
current-channel selector.

[StreamConfigurationTests.cpp](../tests/non_hardware/unit/StreamConfigurationTests.cpp)
covers normalization and rejection without SDK side effects. The
[RX lifecycle](../tests/non_hardware/component/RxLifecycleTests.cpp) and
[TX write](../tests/non_hardware/component/TxWriteStreamTests.cpp) fixtures exercise
opaque-handle lookup before dereference and invalid/foreign-token rejection.
Their fake devices do not reproduce full SDK construction or hardware setup.

`readStreamStatus` reports a stream-relative channel mask: bit zero identifies
the sole channel in that stream, regardless of its logical device channel or
SDK handle. Device-wide time APIs intentionally use the first mapped available
RX or TX handle; those APIs have no channel argument.

## Follow-up boundaries

- Inherited SoapySDR defaults are outside the set of methods implemented here.
  This audit does not add support for unimplemented controls or override the
  base class's behavior for them.
- Draft contributions #80–#83 add per-handle RX gain limits, named RF tuning,
  channel information, and named TX attenuation. Audit their new overloads and
  invalid-channel cases after integration. Existing card-profile RX gain bounds
  and the legacy sample-rate grid are separate capability issues (#80/#86).
- Shared validation does not establish cross-channel clock independence, cache
  propagation, or the safety of live retuning. RX lifecycle serialization and
  physical RX/TX interaction need their own tests.
- Repeat this inventory on the final release tree after source splitting and
  pending API contributions have landed. Passing focused fixtures does not
  replace a native SDK build or hardware validation.
