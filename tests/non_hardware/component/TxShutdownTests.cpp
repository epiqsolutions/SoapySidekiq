/**
 * @file TxShutdownTests.cpp
 * @brief Runs production TX shutdown and device destruction against a tracked SDK.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/TxBlockPool.hpp>
#include <SoapySidekiq/TxCompletionState.hpp>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
constexpr int DEFAULT_NUM_BUFFERS = 3, SOAPY_SDR_ERROR = 1;
using skiq_tx_hdl_t = unsigned;
namespace
{
struct State
{
    std::vector<std::string> events;
    int stop_status{};
    unsigned stops{}, unregisters{}, exits{}, handle{}, card{};
    bool throw_log{}, stopped_after_worker{};
} sdk;
std::atomic<bool> worker_finished{true};
} // namespace
struct skiq_tx_block_t
{
    inline static unsigned live{};
    skiq_tx_block_t()
    {
        ++live;
    }
    ~skiq_tx_block_t()
    {
        --live;
    }
};
skiq_tx_block_t *skiq_tx_block_allocate(std::uint32_t)
{
    return new skiq_tx_block_t;
}
void skiq_tx_block_free(skiq_tx_block_t *block) noexcept
{
    sdk.events.push_back("free-block");
    delete block;
}
int skiq_stop_tx_streaming(std::uint8_t card, skiq_tx_hdl_t handle)
{
    sdk.events.push_back("stop-tx");
    ++sdk.stops;
    sdk.card = card;
    sdk.handle = handle;
    sdk.stopped_after_worker = worker_finished.load();
    return sdk.stop_status;
}
int skiq_stop_tx_streaming_on_1pps(std::uint8_t card, skiq_tx_hdl_t handle, int)
{
    sdk.events.push_back("pps-stop");
    return skiq_stop_tx_streaming(card, handle);
}
int skiq_register_tx_complete_callback(std::uint8_t, std::nullptr_t)
{
    sdk.events.push_back("unregister");
    ++sdk.unregisters;
    return 0;
}
void skiq_exit()
{
    sdk.events.push_back("exit");
    ++sdk.exits;
}
void SoapySDR_logf(int, const char *, ...)
{
    sdk.events.push_back("diagnostic");
    if (sdk.throw_log)
        throw std::runtime_error("injected log failure");
}
struct RxSession
{
    void shutdown() noexcept
    {
        sdk.events.push_back("stop-rx");
    }
};
/** Own real TX state and pool; SDK, RX adapter, and hardware are injected. */
class SoapySidekiq
{
  public:
    struct StreamHandle
    {
        inline static unsigned live{};
        unsigned tx_handle{6};
        bool tx{};
        explicit StreamHandle(bool is_tx) : tx(is_tx)
        {
            ++live;
        }
        ~StreamHandle()
        {
            sdk.events.push_back(tx ? "delete-tx" : "delete-rx");
            --live;
        }
    };
    using TxCompletion = soapy_sidekiq::TxCompletionState<>;
    using TxBlocks = soapy_sidekiq::TxBlockPool<skiq_tx_block_t>;
    std::uint8_t card{3};
    std::uint32_t tx_buffer_count{DEFAULT_NUM_BUFFERS};
    std::uint32_t current_tx_block_size{252}, currTXBuffIndex{};
    std::mutex tx_mutex;
    std::condition_variable tx_cv;
    std::shared_ptr<TxCompletion> tx_completion;
    std::unique_ptr<TxBlocks> tx_blocks;
    bool tx_writes_stopping{}, tx_stream_active{true};
    std::size_t active_tx_writers{}, active_tx_status_readers{};
    std::thread _tx_streaming_thread;
    std::unique_ptr<StreamHandle> active_tx_stream, active_rx_stream;
    std::unique_ptr<RxSession> rx_session{std::make_unique<RxSession>()};
    std::unique_ptr<int> rx_backend{std::make_unique<int>()};
    SoapySidekiq()
    {
        sdk = {};
        sdk.events.reserve(64);
        worker_finished = true;
        initializeTxBuffers();
        active_tx_stream = std::make_unique<StreamHandle>(true);
        active_rx_stream = std::make_unique<StreamHandle>(false);
    }
    ~SoapySidekiq();
    void initializeTxBuffers();
    void stopTxWrites();
    int stopTxStream(bool);
    void shutdownTx() noexcept;
};
#include "TxBufferInitialization.inc"
#include "TxStopWritesMethod.inc"
#include "TxShutdownMethod.inc"
#include "DeviceDestructor.inc"

TEST_CASE(
    "device destruction stops TX and frees unused storage before callback removal and SDK exit")
{
    {
        SoapySidekiq device;
    }
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE_EQ(sdk.handle, 6u);
    REQUIRE_EQ(sdk.card, 3u);
    REQUIRE_EQ(sdk.unregisters, 1u);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
    REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
    const auto position = [](const char *event)
    { return std::find(sdk.events.begin(), sdk.events.end(), event); };
    REQUIRE(position("stop-tx") < position("free-block"));
    REQUIRE(position("delete-tx") < position("unregister"));
    REQUIRE(position("free-block") < position("unregister"));
    REQUIRE(position("stop-rx") < position("unregister"));
    REQUIRE(position("unregister") < position("exit"));
}

TEST_CASE("repeated TX shutdown and later device destruction stop hardware only once")
{
    {
        SoapySidekiq device;
        device.shutdownTx();
        device.shutdownTx();
        REQUIRE_EQ(sdk.stops, 1u);
        REQUIRE(device.tx_writes_stopping);
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.active_tx_stream == nullptr);
        REQUIRE(device.tx_blocks == nullptr);
    }
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
}

TEST_CASE(
    "failed TX stops retain live ownership for retry and already stopped hardware is normalized")
{
    for (const int retry_status : {0, -ENODEV})
    {
        SoapySidekiq device;
        sdk.stop_status = -EIO;
        REQUIRE_EQ(device.stopTxStream(false), -EIO);
        REQUIRE(device.tx_stream_active);
        REQUIRE(device.active_tx_stream != nullptr);
        REQUIRE(device.tx_blocks != nullptr);
        REQUIRE(device.tx_writes_stopping);
        sdk.stop_status = retry_status;
        REQUIRE_EQ(device.stopTxStream(true), 0);
        REQUIRE(!device.tx_stream_active);
        REQUIRE_EQ(device.stopTxStream(false), 0);
        REQUIRE_EQ(sdk.stops, 2u);
    }
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
}

TEST_CASE("failed TX shutdown diagnostics cannot skip remaining device cleanup")
{
    static_assert(noexcept(std::declval<SoapySidekiq &>().shutdownTx()));
    {
        SoapySidekiq device;
        sdk.stop_status = -EIO;
        sdk.throw_log = true;
    }
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE_EQ(sdk.unregisters, 1u);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
    REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
}

TEST_CASE("device destruction joins a pending PPS worker before stopping TX or releasing state")
{
    auto device = std::make_unique<SoapySidekiq>();
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    worker_finished = false;
    device->_tx_streaming_thread = std::thread(
        [&]
        {
            entered.set_value();
            released.wait();
            worker_finished = true;
        });
    entered.get_future().wait();
    auto destruction =
        std::async(std::launch::async, [owned = std::move(device)]() mutable { owned.reset(); });
    const auto pending = destruction.wait_for(30ms);
    release.set_value();
    destruction.get();
    REQUIRE(pending == std::future_status::timeout);
    REQUIRE(sdk.stopped_after_worker);
    REQUIRE_EQ(sdk.stops, 1u);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
    REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
}

TEST_CASE("destruction of configured inactive TX releases storage without a hardware stop")
{
    {
        SoapySidekiq device;
        device.tx_stream_active = false;
    }
    REQUIRE_EQ(sdk.stops, 0u);
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
    REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
}

TEST_CASE("retained accepted block storage can outlive device shutdown and SDK exit")
{
    std::shared_ptr<skiq_tx_block_t> accepted;
    {
        SoapySidekiq device;
        accepted = device.tx_blocks->block(0);
    }
    REQUIRE_EQ(sdk.exits, 1u);
    REQUIRE_EQ(skiq_tx_block_t::live, 1u);
    accepted.reset();
    REQUIRE_EQ(skiq_tx_block_t::live, 0u);
    REQUIRE_EQ(sdk.events.back(), std::string{"free-block"});
}
