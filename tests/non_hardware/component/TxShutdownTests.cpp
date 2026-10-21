/**
 * @file TxShutdownTests.cpp
 * @brief Runs production TX shutdown and device destruction against a tracked SDK.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/TxBlockPool.hpp>
#include <SoapySidekiq/TxCompletionState.hpp>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
constexpr std::uint32_t fixture_tx_buffer_count = 3;
constexpr int SOAPY_SDR_ERROR = 1, SOAPY_SDR_INFO = 2, SOAPY_SDR_STREAM_ERROR = -2;
using skiq_tx_hdl_t = unsigned;
namespace
{
struct State
{
    std::vector<std::string> events;
    int stop_status{}, start_status{};
    unsigned stops{}, starts{}, unregisters{}, exits{}, handle{}, card{}, start_handle{},
        start_card{};
    bool throw_log{}, throw_start{}, stopped_after_worker{};
} sdk;
std::atomic<bool> worker_finished{true};
std::function<void()> pps_start_hook, tx_notify_hook;

class ObservedCondition : public std::condition_variable
{
  public:
    void notify_all() noexcept
    {
        if (tx_notify_hook)
            tx_notify_hook();
        std::condition_variable::notify_all();
    }
};
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
int skiq_start_tx_streaming_on_1pps(std::uint8_t card, skiq_tx_hdl_t handle, int)
{
    ++sdk.starts;
    sdk.start_card = card;
    sdk.start_handle = handle;
    sdk.events.push_back("pps-start");
    if (pps_start_hook)
        pps_start_hook();
    sdk.events.push_back("pps-start-released");
    if (sdk.throw_start)
        throw std::runtime_error("injected startup exception");
    return sdk.start_status;
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
void SoapySDR_log(int level, const char *message)
{
    SoapySDR_logf(level, "%s", message);
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
    struct TxSendContext
    {
        inline static unsigned live{};
        std::shared_ptr<TxCompletion> completion;
        std::shared_ptr<skiq_tx_block_t> block;
        std::uint32_t txIndex{};
        TxSendContext()
        {
            ++live;
        }
        ~TxSendContext()
        {
            --live;
        }
    };
    std::uint8_t card{3};
    std::uint32_t tx_buffer_count{fixture_tx_buffer_count};
    std::uint32_t current_tx_block_size{252}, tx_block_index{};
    std::mutex tx_mutex;
    ObservedCondition tx_cv;
    std::timed_mutex tx_status_mutex;
    bool tx_start_finished{};
    int tx_start_status{};
    std::uint32_t tx_underruns{};
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
    void tx_streaming_start(skiq_tx_hdl_t) noexcept;
    static void tx_complete_callback(std::int32_t, skiq_tx_block_t *, void *) noexcept;
    static void completeTx(const std::shared_ptr<TxCompletion> &, std::uint32_t);
};
#include "TxBufferInitialization.inc"
#include "TxStopWritesMethod.inc"
#include "TxShutdownMethod.inc"
#include "TxStartMethod.inc"
#include "TxCompletionCallback.inc"
#include "TxCompletionMethod.inc"
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

TEST_CASE("device destruction joins actual PPS startup before stop unregister and SDK release")
{
    for (const int start_result : {0, -EIO, 1})
        for (const int stop_result : {0, -EIO})
            for (const bool throwing_logs : {false, true})
            {
                auto device = std::make_unique<SoapySidekiq>();
                auto *const worker_owner = device.get();
                std::weak_ptr<SoapySidekiq::TxCompletion> bookkeeping = device->tx_completion;
                sdk.start_status = start_result == 1 ? 0 : start_result;
                sdk.throw_start = start_result == 1;
                sdk.stop_status = stop_result;
                sdk.throw_log = throwing_logs;
                std::promise<void> entered, release, shutdown_entered;
                const auto edge = release.get_future().share();
                std::atomic<bool> observed_shutdown{false};
                bool released = false, published = false;
                int published_status = 99;
                std::future<void> destruction;
                soapy_sidekiq::ScopeExit cleanup(
                    [&]() noexcept
                    {
                        if (!released)
                            release.set_value();
                        if (destruction.valid())
                            destruction.wait();
                        else if (device && device->_tx_streaming_thread.joinable())
                            device->_tx_streaming_thread.join();
                        pps_start_hook = {};
                        tx_notify_hook = {};
                    });
                pps_start_hook = [&]
                {
                    entered.set_value();
                    edge.wait();
                };
                // Before the held edge, this notification can only come from
                // shutdown closing admission. Avoid a scheduler-based sleep.
                tx_notify_hook = [&]
                {
                    if (!observed_shutdown.exchange(true))
                        shutdown_entered.set_value();
                };
                worker_finished = false;
                device->_tx_streaming_thread = std::thread(
                    [&]
                    {
                        worker_owner->tx_streaming_start(6);
                        published = worker_owner->tx_start_finished;
                        published_status = worker_owner->tx_start_status;
                        worker_finished = true;
                    });
                REQUIRE(entered.get_future().wait_for(2s) == std::future_status::ready);
                destruction = std::async(std::launch::async, [&] { device.reset(); });
                REQUIRE(shutdown_entered.get_future().wait_for(2s) == std::future_status::ready);
                REQUIRE(destruction.wait_for(0ms) == std::future_status::timeout);
                release.set_value();
                released = true;
                destruction.get();
                REQUIRE(published);
                REQUIRE_EQ(published_status,
                           start_result == 1 ? SOAPY_SDR_STREAM_ERROR : start_result);
                REQUIRE(sdk.stopped_after_worker);
                REQUIRE_EQ(sdk.starts, 1u);
                REQUIRE_EQ(sdk.start_card, 3u);
                REQUIRE_EQ(sdk.start_handle, 6u);
                REQUIRE_EQ(sdk.stops, 1u);
                REQUIRE_EQ(sdk.unregisters, 1u);
                REQUIRE_EQ(sdk.exits, 1u);
                REQUIRE(bookkeeping.expired());
                REQUIRE_EQ(skiq_tx_block_t::live, 0u);
                REQUIRE_EQ(SoapySidekiq::StreamHandle::live, 0u);
                const auto position = [](const char *event)
                { return std::find(sdk.events.begin(), sdk.events.end(), event); };
                REQUIRE(position("pps-start-released") < position("stop-tx"));
                REQUIRE(position("stop-tx") < position("delete-tx"));
                REQUIRE(position("delete-tx") < position("unregister"));
                REQUIRE(position("unregister") < position("exit"));
            }
}

TEST_CASE(
    "a delivered late callback releases its context and retained block after device destruction")
{
    for (const bool unexpected_slot : {false, true})
    {
        auto device = std::make_unique<SoapySidekiq>();
        auto context = std::make_unique<SoapySidekiq::TxSendContext>();
        context->completion = device->tx_completion;
        context->block = device->tx_blocks->block(0);
        context->completion->slots[0] = unexpected_slot ? 0 : 1;
        auto *const data = context->block.get();
        std::weak_ptr<SoapySidekiq::TxCompletion> bookkeeping = context->completion;
        std::weak_ptr<skiq_tx_block_t> block = context->block;
        // Model an SDK-accepted context; the callback owns deletion. Always
        // deliver it on assertion failure too, before captured test state ends.
        auto *accepted = context.release();
        soapy_sidekiq::ScopeExit deliver(
            [&]() noexcept
            {
                if (accepted)
                    SoapySidekiq::tx_complete_callback(0, data, accepted);
            });
        sdk.throw_log = true;
        device.reset();
        REQUIRE_EQ(sdk.exits, 1u);
        REQUIRE_EQ(SoapySidekiq::TxSendContext::live, 1u);
        REQUIRE_EQ(skiq_tx_block_t::live, 1u);
        REQUIRE(!bookkeeping.expired());
        REQUIRE(!block.expired());
        auto observed = bookkeeping.lock();
        auto *const completing = accepted;
        accepted = nullptr;
        SoapySidekiq::tx_complete_callback(0, data, completing);
        REQUIRE_EQ(observed->completed, std::uint64_t{1});
        REQUIRE_EQ(observed->slots[0], 0);
        REQUIRE_EQ(SoapySidekiq::TxSendContext::live, 0u);
        REQUIRE_EQ(skiq_tx_block_t::live, 0u);
        REQUIRE(block.expired());
        observed.reset();
        REQUIRE(bookkeeping.expired());
        const auto exit = std::find(sdk.events.begin(), sdk.events.end(), "exit");
        REQUIRE(std::find(exit, sdk.events.end(), "free-block") != sdk.events.end());
    }
}
