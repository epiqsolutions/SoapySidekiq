/**
 * @file RxLifecycleTests.cpp
 * @brief Exercises production RX lifecycle methods with an injected worker failure.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/RxSampleQueue.hpp>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <thread>

#define SOAPY_SDR_RX 1
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_HAS_TIME 4
#define SOAPY_SDR_STREAM_ERROR -2
#define DEFAULT_NUM_BUFFERS 1
#define SoapySDR_logf(...) ((void)0)
#define SoapySDR_log(...) ((void)0)
namespace SoapySDR { struct Stream {}; }
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
struct skiq_tx_block_t {};
void skiq_tx_block_free(skiq_tx_block_t *block) { std::free(block); }

namespace
{
/** Model hardware ownership independently of the worker's running flag. */
struct FakeSdk
{
    bool streaming{};
    int stop_status{};
    unsigned starts{}, stops{};
} sdk;
}

int skiq_start_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    if (sdk.streaming) return -EBUSY;
    sdk.streaming = true;
    ++sdk.starts;
    return 0;
}
int skiq_stop_rx_streaming(std::uint8_t, skiq_rx_hdl_t)
{
    ++sdk.stops;
    if (sdk.stop_status != 0) return sdk.stop_status;
    if (!sdk.streaming) return -ENODEV;
    sdk.streaming = false;
    return 0;
}
int skiq_start_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{ return skiq_start_rx_streaming(card, handle); }
int skiq_stop_rx_streaming_on_1pps(std::uint8_t card, skiq_rx_hdl_t handle, int)
{ return skiq_stop_rx_streaming(card, handle); }
int skiq_stop_tx_streaming(std::uint8_t, skiq_tx_hdl_t) { return 0; }
int skiq_stop_tx_streaming_on_1pps(std::uint8_t, skiq_tx_hdl_t, int) { return 0; }

/** Supply the device fields used by the extracted production lifecycle methods. */
class SoapySidekiq
{
public:
    struct StreamHandle
    {
        int direction{SOAPY_SDR_RX};
        skiq_rx_hdl_t rx_handle{};
        skiq_tx_hdl_t tx_handle{};
        std::size_t channel{};
    };
    std::uint8_t card{};
    std::atomic<bool> rx_running{};
    bool rx_hardware_streaming{}, tx_start_signal{}, tx_stream_active{};
    StreamHandle *active_rx_stream{}, *active_tx_stream{};
    soapy_sidekiq::RxSampleQueue rx_sample_queue{2};
    std::thread _rx_receive_thread, _tx_streaming_thread;
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{};

    SoapySidekiq() { sdk = {}; }
    ~SoapySidekiq()
    {
        if (_rx_receive_thread.joinable()) _rx_receive_thread.join();
        delete active_rx_stream;
    }
    SoapySDR::Stream *setupRx()
    {
        REQUIRE(active_rx_stream == nullptr);
        active_rx_stream = new StreamHandle;
        return reinterpret_cast<SoapySDR::Stream *>(active_rx_stream);
    }
    void awaitWorkerFailure()
    {
        std::int16_t output[2]{};
        REQUIRE(rx_sample_queue.read(output, 1, std::chrono::seconds(1)).status ==
                soapy_sidekiq::RxReadStatus::error);
        REQUIRE(!rx_running.load());
    }
    void rx_receive_operation(skiq_rx_hdl_t, std::size_t);
    void rx_receive_operation_impl(skiq_rx_hdl_t, std::size_t)
    { throw std::runtime_error("injected receive failure"); }
    int activateStream(SoapySDR::Stream *, int, long long, std::size_t);
    int deactivateStream(SoapySDR::Stream *, int, long long);
    void closeStream(SoapySDR::Stream *);
};

static SoapySidekiq::StreamHandle *getStreamHandle(SoapySDR::Stream *stream)
{ return reinterpret_cast<SoapySidekiq::StreamHandle *>(stream); }

// Production worker wrapper, close/deactivate methods, and RX activation branch.
#include "RxLifecycleMethods.inc"

TEST_CASE("closing a failed RX worker stops hardware and permits reactivation")
{
    SoapySidekiq device;
    auto *stream = device.setupRx();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    device.awaitWorkerFailure();
    REQUIRE(sdk.streaming);
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_hardware_streaming);
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE(!device._rx_receive_thread.joinable());
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
    REQUIRE(device.rx_hardware_streaming);
    REQUIRE(device.active_rx_stream != nullptr);
    sdk.stop_status = 0;
    device.closeStream(stream);
    REQUIRE(!sdk.streaming);
    REQUIRE(!device.rx_hardware_streaming);
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
    REQUIRE(!device.rx_hardware_streaming);
    device.closeStream(stream);
    REQUIRE_EQ(sdk.stops, 1u);
}
