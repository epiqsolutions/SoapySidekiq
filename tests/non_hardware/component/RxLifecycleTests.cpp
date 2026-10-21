/**
 * @file RxLifecycleTests.cpp
 * @brief Exercises production RX lifecycle methods with an injected worker failure.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/Diagnostic.hpp>
#include <SoapySidekiq/RxStreamSession.hpp>
#include <SoapySidekiq/TxCompletionState.hpp>
#include <SoapySidekiq/TxBlockPool.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/NumericConversion.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <vector>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <thread>

#define SOAPY_SDR_RX 1
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_HAS_TIME 4
#define SOAPY_SDR_STREAM_ERROR -2
#define SOAPY_SDR_NOT_SUPPORTED -1
#define SOAPY_SDR_TIMEOUT -3
#define SOAPY_SDR_OVERFLOW -4
#define DEFAULT_NUM_BUFFERS 1
std::function<void()> rx_log_hook;
template <typename... Args> void rxLog(const char *, Args &&...)
{
    if (rx_log_hook)
        rx_log_hook();
}
#define SoapySDR_logf(priority, ...) rxLog(__VA_ARGS__)
#define SoapySDR_log(priority, ...) rxLog(__VA_ARGS__)
namespace SoapySDR { struct Stream {}; }
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
struct skiq_tx_block_t {};
void skiq_tx_block_free(skiq_tx_block_t *block) { std::free(block); }

namespace
{
/** RF state indexed by mapped SDK handle, separate from host cache vectors. */
struct RfConfiguration
{
    std::uint32_t rate{1}, bandwidth{1}, actual_bandwidth{1};
    double actual_rate{1.5};
};
/** Model hardware ownership independently of the worker's running flag. */
struct FakeSdk
{
    bool streaming{};
    int stop_status{}, start_status{};
    unsigned starts{}, stops{};
    bool stop_on_pps{}, start_on_pps{}, provide_packet{}, delivered{};
    unsigned tx_starts{}, tx_stops{}, tx_handle{};
    double actual_rate{20000000};
    std::uint64_t system_frequency{1000000}, rf_timestamp{1}, system_timestamp{1};
    unsigned rate_reads{}, frequency_reads{};
    unsigned rate_handle{};
    std::uint32_t configured_rate{}, configured_bandwidth{}, actual_bandwidth{};
    bool omit_rate{};
    int rate_status{}, frequency_status{};
    bool use_rf_table{}, coupled_rf_rates{}, write_before_error{};
    std::array<RfConfiguration, 10> rf;
    unsigned rf_writes{}, rf_write_handle{};
    unsigned fail_rate_read{};
    int rf_write_status{};
} sdk;
std::function<void()> tx_pps_start_hook, rx_pps_start_hook;

int writeRf(unsigned handle, std::uint32_t rate, std::uint32_t bandwidth)
{
    ++sdk.rf_writes;
    sdk.rf_write_handle = handle;
    if (sdk.rf_write_status != 0 && !sdk.write_before_error)
        return sdk.rf_write_status;
    auto &state = sdk.rf.at(handle);
    state.rate = rate;
    state.actual_rate = rate + 0.5;
    state.bandwidth = bandwidth;
    state.actual_bandwidth = bandwidth == 0 ? 1 : bandwidth;
    if (sdk.coupled_rf_rates)
        for (auto &entry : sdk.rf)
        {
            entry.rate = rate;
            entry.actual_rate = rate + 0.5;
        }
    return sdk.rf_write_status;
}
}

int skiq_read_rx_sample_rate_and_bandwidth(std::uint8_t, skiq_rx_hdl_t handle,
                                           std::uint32_t *configured_rate, double *actual_rate,
                                           std::uint32_t *configured_bandwidth,
                                           std::uint32_t *actual_bandwidth)
{
    ++sdk.rate_reads;
    sdk.rate_handle = handle;
    const int status = sdk.rate_reads == sdk.fail_rate_read ? -EIO : sdk.rate_status;
    if (sdk.use_rf_table)
    {
        const auto &state = sdk.rf.at(handle);
        *configured_rate = state.rate;
        *actual_rate = state.actual_rate;
        *configured_bandwidth = state.bandwidth;
        *actual_bandwidth = state.actual_bandwidth;
        return status;
    }
    *configured_rate = sdk.configured_rate;
    if (!sdk.omit_rate)
        *actual_rate = sdk.actual_rate;
    *configured_bandwidth = sdk.configured_bandwidth;
    *actual_bandwidth = sdk.actual_bandwidth;
    return status;
}
int skiq_read_tx_sample_rate_and_bandwidth(std::uint8_t card, skiq_tx_hdl_t handle,
                                           std::uint32_t *rate, double *actual_rate,
                                           std::uint32_t *bandwidth, std::uint32_t *actual_bw)
{
    return skiq_read_rx_sample_rate_and_bandwidth(card, handle, rate, actual_rate, bandwidth,
                                                  actual_bw);
}
int skiq_write_rx_sample_rate_and_bandwidth(std::uint8_t, skiq_rx_hdl_t handle, std::uint32_t rate,
                                            std::uint32_t bandwidth)
{
    return writeRf(handle, rate, bandwidth);
}
int skiq_write_tx_sample_rate_and_bandwidth(std::uint8_t, skiq_tx_hdl_t handle, std::uint32_t rate,
                                            std::uint32_t bandwidth)
{
    return writeRf(handle, rate, bandwidth);
}
int skiq_read_sys_timestamp_freq(std::uint8_t, std::uint64_t *frequency)
{
    ++sdk.frequency_reads;
    *frequency = sdk.system_frequency;
    return sdk.frequency_status;
}
int skiq_start_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    sdk.start_on_pps = false;
    if (sdk.start_status != 0)
        return sdk.start_status;
    if (sdk.streaming) return -EBUSY;
    sdk.streaming = true;
    ++sdk.starts;
    return 0;
}
int skiq_stop_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    ++sdk.stops;
    sdk.stop_on_pps = false;
    if (sdk.stop_status != 0) return sdk.stop_status;
    if (!sdk.streaming) return -ENODEV;
    sdk.streaming = false;
    return 0;
}
int skiq_start_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{
    if (rx_pps_start_hook)
        rx_pps_start_hook();
    const int status = skiq_start_rx_streaming(card, handle);
    sdk.start_on_pps = true;
    return status;
}
int skiq_stop_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{
    const int status = skiq_stop_rx_streaming(card, handle);
    sdk.stop_on_pps = true;
    return status;
}
int skiq_start_tx_streaming_on_1pps(std::uint8_t, skiq_tx_hdl_t handle, int)
{
    ++sdk.tx_starts;
    sdk.tx_handle = handle;
    if (tx_pps_start_hook)
        tx_pps_start_hook();
    return 0;
}
int skiq_stop_tx_streaming(std::uint8_t, skiq_tx_hdl_t)
{
    ++sdk.tx_stops;
    return 0;
}
int skiq_stop_tx_streaming_on_1pps(std::uint8_t card, skiq_tx_hdl_t handle, int)
{
    return skiq_stop_tx_streaming(card, handle);
}

/** Route the real session to the same fake hardware used by the device fixture. */
class FixtureBackend final : public soapy_sidekiq::RxStreamBackend
{
public:
    int start(std::uint32_t handle, bool pps) override
    { return pps ? skiq_start_rx_streaming_on_1pps(0, handle, 0) : skiq_start_rx_streaming(0, handle); }
    int stop(std::uint32_t handle, bool pps) override
    { return pps ? skiq_stop_rx_streaming_on_1pps(0, handle, 0) : skiq_stop_rx_streaming(0, handle); }
    soapy_sidekiq::RxReceiveResult receive() override
    {
        if (!sdk.provide_packet)
            throw std::runtime_error("injected receive failure");
        if (sdk.delivered)
            return {};
        sdk.delivered = true;
        return {soapy_sidekiq::RxReceiveStatus::success,
                sdk.rate_handle,
                {1, 2, 3, 4},
                sdk.rf_timestamp,
                sdk.system_timestamp,
                0};
    }
};

/** Supply the device fields used by the extracted production lifecycle methods. */
class SoapySidekiq
{
public:
  struct Entry
  {
      unsigned handle;
  };
  struct Parameters
  {
      std::array<Entry, 2> rx_param{{{3}, {7}}};
      std::array<Entry, 2> tx_param{{{6}, {2}}};
  } param;
  struct RateAndBandwidth
  {
      std::uint32_t configured_rate;
      double actual_rate;
      std::uint32_t actual_bandwidth;
  };
    struct StreamHandle
    {
        int direction{SOAPY_SDR_RX};
        skiq_rx_hdl_t rx_handle{};
        skiq_tx_hdl_t tx_handle{};
        std::size_t channel{};
    };
    std::uint8_t card{};
    bool tx_start_finished{}, tx_stream_active{}, rfTimeSource{true};
    int tx_start_status{};
    std::uint32_t tx_underruns{};
    std::timed_mutex tx_status_mutex;
    std::mutex tx_mutex;
    std::shared_ptr<soapy_sidekiq::TxCompletionState<>> tx_completion{
        std::make_shared<soapy_sidekiq::TxCompletionState<>>(3)};
    std::condition_variable tx_cv;
    bool tx_writes_stopping{true};
    std::size_t active_tx_writers{}, active_tx_status_readers{};
    void stopTxWrites();
    int stopTxStream(bool);
    void tx_streaming_start(skiq_tx_hdl_t) noexcept;
    std::uint32_t rx_payload_size_in_words{2}, current_tx_block_size{252};
    bool rxUseShort{true};
    double maxValue{2048};
    std::uint64_t sys_freq{1000000};
    std::uint8_t num_rx_channels{2}, num_tx_channels{2};
    std::vector<std::uint32_t> rx_sample_rates{20000000, 20000000};
    std::vector<std::uint32_t> tx_sample_rates{20000000, 20000000};
    std::vector<std::uint32_t> rx_bandwidths{18000000, 18000000};
    std::vector<std::uint32_t> tx_bandwidths{18000000, 18000000};
    std::unique_ptr<StreamHandle> active_rx_stream, active_tx_stream;
    FixtureBackend backend;
    std::unique_ptr<soapy_sidekiq::RxStreamSession> rx_session;
    std::thread _tx_streaming_thread;
    std::unique_ptr<soapy_sidekiq::TxBlockPool<skiq_tx_block_t>> tx_blocks;

    SoapySidekiq()
    {
        sdk = {};
        rx_session = std::make_unique<soapy_sidekiq::RxStreamSession>(backend);
    }
    ~SoapySidekiq()
    {
        rx_session.reset();
        active_rx_stream.reset();
    }
    SoapySDR::Stream *setupRx()
    {
        REQUIRE(active_rx_stream == nullptr);
        active_rx_stream = std::make_unique<StreamHandle>();
        return reinterpret_cast<SoapySDR::Stream *>(active_rx_stream.get());
    }
    void awaitWorkerFailure()
    {
        std::int16_t output[2]{};
        REQUIRE(rx_session->read(output, 1, std::chrono::seconds(1)).status ==
                soapy_sidekiq::RxReadStatus::error);
        REQUIRE(!rx_session->running());
    }
    StreamHandle *findStreamHandle(SoapySDR::Stream *) const noexcept;
    std::size_t getStreamMTU(SoapySDR::Stream *) const;
    int readStream(SoapySDR::Stream *, void *const *, std::size_t, int &, long long &, long);
    int readStreamImpl(SoapySDR::Stream *, void *const *, std::size_t, int &, long long &, long);
    void configureWhileRxInactive(const std::function<void()> &);
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    void setSampleRate(int, std::size_t, double);
    double getSampleRate(int, std::size_t) const;
    void setBandwidth(int, std::size_t, double);
    double getBandwidth(int, std::size_t) const;
    void initializeRateCaches();
    RateAndBandwidth readRateAndBandwidth(int, std::size_t) const;
    void applyRateAndBandwidth(int, std::size_t, std::uint32_t, std::uint32_t);
    int activateStream(SoapySDR::Stream *, int, long long, std::size_t);
    int activateStreamImpl(SoapySDR::Stream *, int, long long, std::size_t);
    int deactivateStream(SoapySDR::Stream *, int, long long);
    void closeStream(SoapySDR::Stream *);
};

#include "StreamHandleLookup.inc"
#include "StreamMtuMethod.inc"
#include "StreamReadMethod.inc"

// Production close/deactivate and RX activation, using the real session worker.
#include "RxLifecycleMethods.inc"
#include "TxStopWritesMethod.inc"
#include "TxStartMethod.inc"
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "RateBandwidthMethods.inc"

TEST_CASE("closing a failed RX worker stops hardware and permits reactivation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE(sdk.streaming);
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_session->running());
    REQUIRE_EQ(sdk.stops, 1u);
    stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    device.closeStream(stream);
    REQUIRE_EQ(sdk.starts, 2u);
    REQUIRE_EQ(sdk.stops, 2u);
}

TEST_CASE("failed RX stop preserves ownership for a successful close retry")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    sdk.stop_status = -EIO;
    REQUIRE_THROWS_AS(device.closeStream(stream), std::runtime_error);
    REQUIRE(sdk.streaming);
    REQUIRE_THROWS_AS(device.rx_session->activate({0, 2, 20000000, 1000000, true, false}),
                      std::logic_error);
    REQUIRE(device.active_rx_stream != nullptr);
    sdk.stop_status = 0;
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_session->running());
    REQUIRE_EQ(sdk.stops, 2u);
}

TEST_CASE("RX close handles already-stopped hardware without redundant deactivation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    sdk.streaming = false;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE(!device.rx_session->running());
    device.closeStream(stream);
    REQUIRE_EQ(sdk.stops, 1u);
}

TEST_CASE("immediate RX stop is independent of PPS activation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE(!sdk.stop_on_pps);
    device.closeStream(stream);
}

TEST_CASE("PPS RX stop is independent of immediate activation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 0), 0);
    REQUIRE(sdk.stop_on_pps);
    device.closeStream(stream);
}

TEST_CASE("closing a failed PPS RX stream requests immediate hardware cleanup")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
    device.awaitWorkerFailure();
    device.closeStream(stream);
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE(!sdk.stop_on_pps);
}

TEST_CASE("session destruction after PPS activation requests immediate hardware cleanup")
{
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
        device.awaitWorkerFailure();
    }
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE(!sdk.stop_on_pps);
}

TEST_CASE("RX stream APIs reject null inaccessible and foreign tokens before dereference")
{
    SoapySidekiq device;
    SoapySidekiq other;
    auto *live = device.setupRx();
    auto *foreign = other.setupRx();
    int flags = 123;
    long long timestamp = 456;
    for (auto *invalid : {static_cast<SoapySDR::Stream *>(nullptr),
                          reinterpret_cast<SoapySDR::Stream *>(std::uintptr_t{1}), foreign})
    {
        REQUIRE_EQ(device.activateStream(invalid, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.deactivateStream(invalid, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.readStream(invalid, nullptr, 1, flags, timestamp, 0),
                   SOAPY_SDR_STREAM_ERROR);
        REQUIRE_THROWS_AS(device.getStreamMTU(invalid), std::invalid_argument);
        REQUIRE_THROWS_AS(device.closeStream(invalid), std::invalid_argument);
    }
    REQUIRE_EQ(sdk.starts + sdk.stops, 0u);
    REQUIRE_EQ(flags, 123);
    REQUIRE_EQ(timestamp, 456LL);
    REQUIRE_EQ(device.getStreamMTU(live), std::size_t{2});
}
TEST_CASE("closed RX tokens are rejected without accessing freed storage")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    device.closeStream(stream);
    int flags = 0;
    long long timestamp = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStream(stream, nullptr, 1, flags, timestamp, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_THROWS_AS(device.getStreamMTU(stream), std::invalid_argument);
    REQUIRE_THROWS_AS(device.closeStream(stream), std::invalid_argument);
}

TEST_CASE("stream MTU reports positive sizes and rejects an invalid direction")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.getStreamMTU(stream), std::size_t{2});
    device.active_rx_stream->direction = SOAPY_SDR_TX;
    REQUIRE_EQ(device.getStreamMTU(stream), std::size_t{252});
    device.active_rx_stream->direction = 99;
    REQUIRE_THROWS_AS(device.getStreamMTU(stream), std::invalid_argument);
    device.active_rx_stream->direction = SOAPY_SDR_RX;
}

TEST_CASE("RX activation refreshes actual fractional rate instead of the stale cache")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.actual_rate = 1.5;
    device.active_rx_stream->channel = 1;
    device.active_rx_stream->rx_handle = 7;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    const auto first = device.rx_session->read(output, 1, std::chrono::seconds(1));
    REQUIRE_EQ(first.time_ns, std::int64_t{666666666});
    const auto second = device.rx_session->read(output, 1, std::chrono::seconds(1));
    REQUIRE_EQ(second.time_ns, std::int64_t{1333333333});
    REQUIRE_EQ(sdk.rate_handle, 7u);
    REQUIRE_EQ(sdk.rate_reads, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX activation refreshes the selected system clock before starting hardware")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.system_frequency = 3;
    device.rfTimeSource = false;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    REQUIRE_EQ(device.rx_session->read(output, 1, std::chrono::seconds(1)).time_ns,
               std::int64_t{333333333});
    REQUIRE_EQ(sdk.frequency_reads, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX activation validates refreshed rates before any hardware start")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.rate_status = -5;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    sdk.rate_status = 0;
    sdk.actual_rate = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    sdk.actual_rate = 20000000;
    device.rfTimeSource = false;
    sdk.frequency_status = -6;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    sdk.frequency_status = 0;
    sdk.system_frequency = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(sdk.starts, 0u);
}

TEST_CASE("RX timestamp setup accepts actual rate without configured rate or bandwidth")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    sdk.actual_rate = 1.5;
    sdk.configured_rate = sdk.configured_bandwidth = sdk.actual_bandwidth = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    std::int16_t output[2]{};
    REQUIRE_EQ(device.rx_session->read(output, 1, std::chrono::seconds(1)).time_ns,
               std::int64_t{666666666});
    REQUIRE_EQ(sdk.starts, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("RX timestamp setup rejects omitted actual rate before hardware start")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.omit_rate = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(sdk.starts, 0u);
}

TEST_CASE("activation rejects unsupported flags bursts and absolute times before starting")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        device.active_rx_stream->direction = direction;
        for (const int flags : {1, 2, SOAPY_SDR_HAS_TIME | 2, -1})
            REQUIRE_EQ(device.activateStream(stream, flags, 0, 0), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 1), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 1),
                   SOAPY_SDR_NOT_SUPPORTED);
        for (const long long time : {-1LL, 1LL, 1000000000LL})
            REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, time, 0),
                       SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(sdk.starts + sdk.stops + sdk.rate_reads + sdk.frequency_reads, 0u);
        device.active_rx_stream->direction = SOAPY_SDR_RX;
    }
}
TEST_CASE("RX stop rejects unsupported options without stopping the active session")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    for (const int flags : {1, SOAPY_SDR_HAS_TIME | 2, -1})
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 1), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(sdk.stops, 0u);
    REQUIRE(device.rx_session->running());
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
}
TEST_CASE("immediate RX activation ignores timeNs when HAS_TIME is absent")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.provide_packet = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 123, 0), 0);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 123), 0);
}

TEST_CASE("RX startup failures return stream errors and retained hardware cannot be started twice")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    sdk.start_status = -EIO;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE(!sdk.streaming);
    REQUIRE_EQ(sdk.starts, 0u);
    sdk.start_status = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE(sdk.streaming);
    REQUIRE_EQ(sdk.starts, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    device.closeStream(stream);
}

TEST_CASE("failed RX deactivation returns a stream error and preserves hardware stop retry")
{
    for (const int flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = device.setupRx();
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        device.awaitWorkerFailure();
        auto *const owner = device.active_rx_stream.get();
        sdk.stop_status = -EIO;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE(device.active_rx_stream.get() == owner);
        REQUIRE(sdk.streaming);
        REQUIRE_EQ(sdk.stop_on_pps, flags == SOAPY_SDR_HAS_TIME);
        REQUIRE_THROWS_AS(device.closeStream(stream), std::runtime_error);
        REQUIRE(device.active_rx_stream.get() == owner);
        REQUIRE(sdk.streaming);
        sdk.stop_status = 0;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), 0);
        REQUIRE(!sdk.streaming);
        device.closeStream(stream);
        REQUIRE(device.active_rx_stream == nullptr);
    }
}

TEST_CASE("RX activation preserves error status and successful retry when diagnostics throw")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    rx_log_hook = [] { throw std::runtime_error("injected diagnostic failure"); };
    soapy_sidekiq::ScopeExit restore([]() noexcept { rx_log_hook = {}; });
    sdk.actual_rate = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(sdk.starts, 0u);
    sdk.actual_rate = 20000000;
    sdk.provide_packet = true;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    REQUIRE_EQ(sdk.starts, 1u);
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    rx_log_hook = {};
    device.closeStream(stream);
}

TEST_CASE("RX partial timestamp overflow returns stream error even when diagnostics throw")
{
    for (const bool native : {true, false})
    {
        SoapySidekiq device;
        device.rxUseShort = native;
        auto *stream = device.setupRx();
        sdk.provide_packet = true;
        sdk.actual_rate = 1;
        // First sample fits int64 nanoseconds; the next sample advances beyond it.
        sdk.rf_timestamp = 9223372036ULL;
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        std::int16_t shorts[2]{};
        float floats[2]{};
        void *buffers[]{native ? static_cast<void *>(shorts) : static_cast<void *>(floats)};
        int flags = 0;
        long long timestamp = 0;
        REQUIRE_EQ(device.readStream(stream, buffers, 1, flags, timestamp, 1000000), 1);
        REQUIRE_EQ(timestamp, 9223372036000000000LL);
        shorts[0] = 97;
        floats[0] = 97;
        unsigned diagnostics = 0;
        rx_log_hook = [&]
        {
            ++diagnostics;
            throw std::runtime_error("injected diagnostic failure");
        };
        soapy_sidekiq::ScopeExit reset([]() noexcept { rx_log_hook = {}; });
        REQUIRE_EQ(device.readStream(stream, buffers, 1, flags, timestamp, 0),
                   SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(flags, 0);
        REQUIRE_EQ(timestamp, 0LL);
        REQUIRE_EQ(diagnostics, 1u);
        REQUIRE_EQ(shorts[0], 97);
        REQUIRE_EQ(floats[0], 97.0f);
        rx_log_hook = {};
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        sdk.rf_timestamp = 1;
        sdk.delivered = false;
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        REQUIRE_EQ(device.readStream(stream, buffers, 1, flags, timestamp, 1000000), 1);
        REQUIRE_EQ(timestamp, 1000000000LL);
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        device.closeStream(stream);
    }
}

TEST_CASE("device configuration guard honors retained RX hardware after worker and stop failures")
{
    SoapySidekiq device;
    bool changed = false;
    device.configureWhileRxInactive([&] { changed = true; });
    REQUIRE(changed);
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    changed = false;
    REQUIRE_THROWS_AS(device.configureWhileRxInactive([&] { changed = true; }), std::logic_error);
    REQUIRE(!changed);
    sdk.stop_status = -EIO;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_THROWS_AS(device.configureWhileRxInactive([&] { changed = true; }), std::logic_error);
    REQUIRE(!changed);
    sdk.stop_status = 0;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    device.configureWhileRxInactive([&] { changed = true; });
    REQUIRE(changed);
    device.closeStream(stream);
}

TEST_CASE("RX PPS activation and failure recovery preserve a concurrently started TX stream")
{
    using namespace std::chrono_literals;
    for (const bool fail_rx_start : {false, true})
        for (const int stop_flags : {0, SOAPY_SDR_HAS_TIME})
        {
            SoapySidekiq device;
            auto *rx_stream = device.setupRx();
            device.active_rx_stream->rx_handle = 7;
            device.active_rx_stream->channel = 1;
            device.active_tx_stream = std::make_unique<SoapySidekiq::StreamHandle>();
            device.active_tx_stream->direction = SOAPY_SDR_TX;
            device.active_tx_stream->tx_handle = 6;
            auto *const tx_owner = device.active_tx_stream.get();
            device.tx_stream_active = true;
            device.tx_underruns = 99;
            sdk.provide_packet = true;
            sdk.start_status = fail_rx_start ? -EIO : 0;
            std::promise<void> tx_entered, rx_entered, release_tx, release_rx, tx_completed,
                rx_waiting_for_edge;
            const auto tx_edge = release_tx.get_future().share();
            const auto rx_edge = release_rx.get_future().share();
            const auto tx_done = tx_completed.get_future().share();
            bool tx_released = false, rx_released = false;
            std::future<int> activation;
            soapy_sidekiq::ScopeExit cleanup(
                [&]() noexcept
                {
                    if (!tx_released)
                        release_tx.set_value();
                    if (!rx_released)
                        release_rx.set_value();
                    if (activation.valid())
                        activation.wait();
                    if (device._tx_streaming_thread.joinable())
                        device._tx_streaming_thread.join();
                    tx_pps_start_hook = {};
                    rx_pps_start_hook = {};
                });
            tx_pps_start_hook = [&]
            {
                tx_entered.set_value();
                tx_edge.wait();
            };
            rx_pps_start_hook = [&]
            {
                rx_entered.set_value();
                // Model an SDK serializing the RX PPS operation behind TX.
                // Bound this wait so a lock-order regression can unwind cleanly.
                if (tx_done.wait_for(2s) != std::future_status::ready)
                    throw std::runtime_error("TX worker could not publish while RX startup waited");
                rx_waiting_for_edge.set_value();
                rx_edge.wait();
            };
            device._tx_streaming_thread = std::thread(
                [&]
                {
                    device.tx_streaming_start(6);
                    tx_completed.set_value();
                });
            REQUIRE(tx_entered.get_future().wait_for(2s) == std::future_status::ready);
            activation =
                std::async(std::launch::async, [&]
                           { return device.activateStream(rx_stream, SOAPY_SDR_HAS_TIME, 0, 0); });
            REQUIRE(rx_entered.get_future().wait_for(2s) == std::future_status::ready);
            REQUIRE(activation.wait_for(0ms) == std::future_status::timeout);
            release_tx.set_value();
            tx_released = true;
            REQUIRE(rx_waiting_for_edge.get_future().wait_for(2s) == std::future_status::ready);
            device._tx_streaming_thread.join();
            REQUIRE(device.tx_start_finished);
            REQUIRE_EQ(device.tx_start_status, 0);
            REQUIRE_EQ(device.tx_underruns, 0u);
            REQUIRE_EQ(sdk.tx_starts, 1u);
            REQUIRE_EQ(sdk.tx_handle, 6u);
            REQUIRE(activation.wait_for(0ms) == std::future_status::timeout);
            release_rx.set_value();
            rx_released = true;
            REQUIRE_EQ(activation.get(), fail_rx_start ? SOAPY_SDR_STREAM_ERROR : 0);
            REQUIRE(sdk.start_on_pps);
            REQUIRE(device.tx_stream_active);
            REQUIRE(device.active_tx_stream.get() == tx_owner);
            REQUIRE_EQ(sdk.tx_stops, 0u);
            if (fail_rx_start)
            {
                REQUIRE(!sdk.streaming);
                sdk.start_status = 0;
                REQUIRE_EQ(device.activateStream(rx_stream, 0, 0, 0), 0);
                REQUIRE(!sdk.start_on_pps);
            }
            std::int16_t output[2]{};
            void *buffers[]{output};
            int flags = 0;
            long long timestamp = 0;
            REQUIRE_EQ(device.readStream(rx_stream, buffers, 1, flags, timestamp, 1000000), 1);
            REQUIRE_EQ(output[0], 1);
            REQUIRE_EQ(output[1], 2);
            REQUIRE_EQ(timestamp, 50LL);
            REQUIRE_EQ(flags, SOAPY_SDR_HAS_TIME);
            REQUIRE_EQ(sdk.rate_handle, 7u);
            REQUIRE_EQ(device.deactivateStream(rx_stream, stop_flags, 0), 0);
            REQUIRE_EQ(sdk.stop_on_pps, stop_flags == SOAPY_SDR_HAS_TIME);
            device.closeStream(rx_stream);
            REQUIRE(device.active_rx_stream == nullptr);
            REQUIRE(device.tx_stream_active);
            REQUIRE(device.active_tx_stream.get() == tx_owner);
            REQUIRE_EQ(sdk.tx_stops, 0u);
            REQUIRE_EQ(device.stopTxStream(false), 0);
            REQUIRE(!device.tx_stream_active);
            REQUIRE_EQ(sdk.tx_stops, 1u);
        }
}

namespace
{
// Run the production TX startup body; fake SDK startup returns immediately here.
void startRetuneFixtureTx(SoapySidekiq &device)
{
    device.active_tx_stream = std::make_unique<SoapySidekiq::StreamHandle>();
    device.active_tx_stream->direction = SOAPY_SDR_TX;
    device.active_tx_stream->tx_handle = device.getTxHandle(1);
    device.tx_stream_active = true;
    device.tx_writes_stopping = false;
    std::thread worker([&] { device.tx_streaming_start(device.getTxHandle(1)); });
    worker.join();
}
} // namespace

TEST_CASE("RF transactions use the real RX ownership gate and restart from coupled readback")
{
    for (const std::size_t channel : {std::size_t{0}, std::size_t{1}})
    {
        SoapySidekiq device;
        sdk.use_rf_table = sdk.coupled_rf_rates = sdk.provide_packet = true;
        auto *const stream = device.setupRx();
        device.active_rx_stream->channel = channel;
        device.active_rx_stream->rx_handle = device.getRxHandle(channel);
        startRetuneFixtureTx(device);
        auto *const tx_owner = device.active_tx_stream.get();
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        std::int16_t output[2]{};
        REQUIRE_EQ(device.rx_session->read(output, 1, std::chrono::seconds(1)).time_ns,
                   std::int64_t{666666666});

        const auto reads = sdk.rate_reads;
        const auto check_blocked = [&]
        {
            for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
                for (const std::size_t target : {std::size_t{0}, std::size_t{1}})
                {
                    REQUIRE_THROWS_AS(device.setSampleRate(direction, target, 3), std::logic_error);
                    REQUIRE_THROWS_AS(device.setBandwidth(direction, target, 1), std::logic_error);
                }
            REQUIRE_EQ(sdk.rate_reads, reads);
            REQUIRE_EQ(sdk.rf_writes, 0u);
        };
        check_blocked();
        sdk.stop_status = -EIO;
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), SOAPY_SDR_STREAM_ERROR);
        // Worker exit is insufficient: a failed SDK stop retains the same RF gate.
        REQUIRE(!device.rx_session->running());
        check_blocked();
        sdk.stop_status = 0;
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);

        device.setSampleRate(SOAPY_SDR_TX, 1, 3);
        REQUIRE_EQ(sdk.rf_write_handle, 2u);
        REQUIRE_EQ(device.rx_sample_rates[channel], 20000000u);
        REQUIRE_EQ(device.getSampleRate(SOAPY_SDR_RX, channel), 3.5);
        device.setBandwidth(SOAPY_SDR_RX, channel, 1);
        REQUIRE_EQ(sdk.rf_write_handle, device.getRxHandle(channel));
        REQUIRE_EQ(sdk.rf.at(device.getRxHandle(channel)).rate, 3u);
        REQUIRE_EQ(device.rx_sample_rates[channel], 3u);
        REQUIRE_EQ(device.rx_sample_rates[1 - channel], 20000000u);

        sdk.delivered = false;
        sdk.rf_timestamp = 7;
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        const auto first = device.rx_session->read(output, 1, std::chrono::seconds(1));
        const auto second = device.rx_session->read(output, 1, std::chrono::seconds(1));
        REQUIRE(first.status == soapy_sidekiq::RxReadStatus::samples);
        REQUIRE(second.status == soapy_sidekiq::RxReadStatus::samples);
        REQUIRE_EQ(first.time_ns, std::int64_t{2000000000});
        REQUIRE_EQ(second.time_ns, std::int64_t{2285714285});
        REQUIRE_EQ(output[0], std::int16_t{3});
        REQUIRE_EQ(output[1], std::int16_t{4});
        REQUIRE_EQ(sdk.rate_handle, device.getRxHandle(channel));
        REQUIRE(device.active_tx_stream.get() == tx_owner);
        REQUIRE(device.tx_stream_active);
        REQUIRE(!device.tx_writes_stopping);
        REQUIRE_EQ(sdk.tx_starts, 1u);
        REQUIRE_EQ(sdk.tx_stops, 0u);
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        device.closeStream(stream);
        REQUIRE_EQ(sdk.tx_stops, 0u);
        REQUIRE_EQ(device.stopTxStream(false), 0);
        REQUIRE_EQ(sdk.tx_stops, 1u);
    }
}

TEST_CASE("RX restart refreshes coupled hardware after failed RF writes or post-write readback")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{0}, std::size_t{1}})
            for (const bool write_error : {false, true})
            {
                SoapySidekiq device;
                sdk.use_rf_table = sdk.coupled_rf_rates = sdk.provide_packet = true;
                auto *const stream = device.setupRx();
                device.active_rx_stream->channel = channel;
                device.active_rx_stream->rx_handle = device.getRxHandle(channel);
                startRetuneFixtureTx(device);
                auto *const tx_owner = device.active_tx_stream.get();
                REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
                REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
                const auto rx_rates = device.rx_sample_rates;
                const auto tx_rates = device.tx_sample_rates;
                const auto rx_widths = device.rx_bandwidths;
                const auto tx_widths = device.tx_bandwidths;
                if (write_error)
                {
                    sdk.write_before_error = true;
                    sdk.rf_write_status = -EIO;
                }
                else
                    sdk.fail_rate_read = sdk.rate_reads + 2; // Prefetch succeeds, readback fails.
                REQUIRE_THROWS_AS(device.setSampleRate(direction, channel, 3), std::runtime_error);
                REQUIRE_EQ(sdk.rf_writes, 1u);
                REQUIRE(device.rx_sample_rates == rx_rates);
                REQUIRE(device.tx_sample_rates == tx_rates);
                REQUIRE(device.rx_bandwidths == rx_widths);
                REQUIRE(device.tx_bandwidths == tx_widths);
                sdk.rf_write_status = 0;
                sdk.fail_rate_read = 0;
                REQUIRE_EQ(device.getSampleRate(SOAPY_SDR_RX, channel), 3.5);
                sdk.delivered = false;
                sdk.rf_timestamp = 7;
                REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
                std::int16_t output[2]{};
                const auto read = device.rx_session->read(output, 1, std::chrono::seconds(1));
                REQUIRE(read.status == soapy_sidekiq::RxReadStatus::samples);
                REQUIRE_EQ(read.time_ns, std::int64_t{2000000000});
                REQUIRE(device.active_tx_stream.get() == tx_owner);
                REQUIRE(device.tx_stream_active);
                REQUIRE_EQ(sdk.tx_stops, 0u);
                REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
                REQUIRE_EQ(device.stopTxStream(false), 0);
            }
}
