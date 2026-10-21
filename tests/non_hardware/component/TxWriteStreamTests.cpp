/**
 * @file TxWriteStreamTests.cpp
 * @brief Verifies production TX writes, completion, and PPS startup with a scripted SDK.
 */

#include "TestHarness.hpp"

#include <cstddef>
#include <chrono>
#include <limits>
#include <atomic>
#include <algorithm>
#include <functional>
#include <future>
#include <cerrno>
#include <new>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/TxCompletionState.hpp>
#include <SoapySidekiq/TxBlockPool.hpp>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <SoapySidekiq/SampleScale.hpp>
#include <SoapySidekiq/SampleConversion.hpp>
#include <SoapySidekiq/StreamConfiguration.hpp>
#include <vector>

// Sends may complete inline or later; a controlled PPS call can wait for a test edge.
// Stand-ins match the members used by writeStream without opening a real device.
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SOAPY_SDR_HAS_TIME 4
#define SOAPY_SDR_NOT_SUPPORTED -1
#define SOAPY_SDR_STREAM_ERROR -2
#define SOAPY_SDR_TIMEOUT -3
#define SOAPY_SDR_UNDERFLOW -4
#define SKIQ_TX_ASYNC_SEND_QUEUE_FULL 1
#define DEFAULT_NUM_BUFFERS 3
std::function<void()> log_hook;
static void completionLog()
{
    if (log_hook)
        log_hook();
}
#define SoapySDR_logf(...) completionLog()
#define SoapySDR_log(...) completionLog()

namespace SoapySDR { struct Stream {}; }
struct skiq_tx_block_t
{
    inline static std::atomic<std::size_t> live{};
    std::int16_t data[504]{};
    skiq_tx_block_t()
    {
        ++live;
    }
    ~skiq_tx_block_t()
    {
        --live;
    }
};
std::size_t block_allocations{}, fail_block_allocation = std::numeric_limits<std::size_t>::max();
skiq_tx_block_t *skiq_tx_block_allocate(std::uint32_t samples)
{
    REQUIRE_EQ(samples, 252u);
    if (block_allocations++ == fail_block_allocation)
        return nullptr;
    return new skiq_tx_block_t;
}
void skiq_tx_block_free(skiq_tx_block_t *block) noexcept
{
    delete block;
}
class SoapySidekiq;
struct TxCompletion;
struct passedStruct
{
    inline static std::atomic<std::size_t> live{};
    inline static bool fail_allocation{};
    std::shared_ptr<TxCompletion> completion;
    std::shared_ptr<skiq_tx_block_t> block;
    std::uint32_t txIndex{};
    passedStruct()
    {
        ++live;
    }
    ~passedStruct()
    {
        --live;
    }
    static void *operator new(std::size_t bytes)
    {
        if (fail_allocation)
            throw std::bad_alloc();
        return ::operator new(bytes);
    }
    static void operator delete(void *pointer) noexcept
    {
        ::operator delete(pointer);
    }
};
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *, passedStruct *);
using skiq_tx_hdl_t = unsigned;
using skiq_rx_hdl_t = unsigned;
int stop_status{}, stop_calls{};
std::function<void()> stop_hook;
int skiq_stop_tx_streaming(std::uint8_t, skiq_tx_hdl_t)
{
    if (stop_hook)
        stop_hook();
    ++stop_calls;
    return stop_status;
}
int skiq_stop_tx_streaming_on_1pps(std::uint8_t card, skiq_tx_hdl_t handle, int)
{
    return skiq_stop_tx_streaming(card, handle);
}
// Script configuration and immediate-start failures at their actual SDK boundary.
constexpr int skiq_iq_order_iq = 0, skiq_tx_immediate_data_flow_mode = 0,
              skiq_tx_transfer_mode_async = 0;
std::vector<std::string> activation_calls;
std::size_t activation_fail_at{};
unsigned activation_handle{};
int recordActivation(const char *operation)
{
    activation_calls.emplace_back(operation);
    return activation_calls.size() == activation_fail_at ? -EIO : 0;
}
int skiq_write_iq_order_mode(std::uint8_t, int)
{
    return recordActivation("iq_order");
}
int skiq_write_tx_block_size(std::uint8_t, unsigned handle, std::uint32_t)
{
    activation_handle = handle;
    return recordActivation("block_size");
}
int skiq_write_tx_data_flow_mode(std::uint8_t, unsigned, int)
{
    return recordActivation("data_flow");
}
int skiq_write_tx_transfer_mode(std::uint8_t, unsigned, int)
{
    return recordActivation("transfer_mode");
}
int skiq_write_num_tx_threads(std::uint8_t, unsigned)
{
    return recordActivation("threads");
}
int skiq_start_tx_streaming(std::uint8_t, unsigned)
{
    return recordActivation("start");
}
int pps_start_status{};
std::function<void()> pps_start_hook;
int skiq_start_tx_streaming_on_1pps(std::uint8_t, skiq_tx_hdl_t, int)
{
    if (pps_start_hook)
        pps_start_hook();
    return pps_start_status;
}
std::vector<std::uint32_t> underrun_counts;
std::size_t underrun_reads{};
int underrun_status{};
std::function<void()> underrun_hook;
unsigned underrun_handle{};
int skiq_read_tx_num_underruns(std::uint8_t, skiq_tx_hdl_t handle, std::uint32_t *errors)
{
    underrun_handle = handle;
    if (underrun_hook)
        underrun_hook();
    const auto index = underrun_reads++;
    *errors =
        underrun_counts.empty() ? 0 : underrun_counts[std::min(index, underrun_counts.size() - 1)];
    return underrun_status;
}
struct UnusedRxSession
{
    bool running() const
    {
        return false;
    }
    int deactivate(bool)
    {
        return 0;
    }
};

// Observe wait boundaries without sleeps; injected completion runs under the
// same locking protocol as a real SDK callback, on another thread.
std::function<void()> wait_hook;
class FixtureCondition
{
    std::condition_variable condition;

  public:
    std::vector<std::chrono::steady_clock::time_point> deadlines;
    void notify_all()
    {
        condition.notify_all();
    }
    template <typename Predicate> void wait(std::unique_lock<std::mutex> &lock, Predicate ready)
    {
        condition.wait(lock, ready);
    }
    template <typename Predicate>
    bool wait_until(std::unique_lock<std::mutex> &lock,
                    std::chrono::steady_clock::time_point deadline, Predicate ready)
    {
        deadlines.push_back(deadline);
        if (!wait_hook || ready())
            return condition.wait_until(lock, deadline, ready);
        auto hook = std::move(wait_hook);
        wait_hook = {};
        std::thread completion(
            [&, hook = std::move(hook)]
            {
                hook();
                condition.notify_all();
            });
        const bool result = condition.wait_until(lock, deadline, ready);
        // A timeout may race the completion acquiring this mutex. Let it finish
        // before joining, then restore the condition-variable lock contract.
        lock.unlock();
        completion.join();
        lock.lock();
        return result;
    }
};

/** Extend only the SDK fixture lookup, keeping real completion storage/locking. */
struct TxCompletion : soapy_sidekiq::TxCompletionState<FixtureCondition>
{
    explicit TxCompletion(std::size_t count) : TxCompletionState(count) {}
    SoapySidekiq *fixture_owner{};
};

/** Minimal device fixture supplying the state accessed by the production method. */
class SoapySidekiq
{
public:
  struct StreamHandle
  {
      int direction{SOAPY_SDR_TX};
      unsigned tx_handle{}, rx_handle{};
  };
  std::unique_ptr<StreamHandle> active_tx_stream{std::make_unique<StreamHandle>()};
  StreamHandle &handle{*active_tx_stream};
  std::unique_ptr<StreamHandle> active_rx_stream;
  UnusedRxSession *rx_session{};
  bool tx_stream_active{true};
  bool tx_writes_stopping{};
  std::size_t active_tx_writers{}, active_tx_status_readers{};
  std::thread _tx_streaming_thread;
  bool first_transmit{false}, txUseShort{false}, iq_swap{true};
  std::uint32_t p_tx_block_index{};
  std::uint32_t tx_buffer_count{DEFAULT_NUM_BUFFERS};
  std::uint32_t current_tx_block_size{252}, currTXBuffIndex{};
  std::uint8_t card{};
  double maxValue{32767};
  double txMaxValue{32767};
  std::uint8_t num_rx_channels{1}, num_tx_channels{1};
  std::size_t getNumChannels(int) const;
  void validateChannel(int, std::size_t) const;
  using TxBlocks = soapy_sidekiq::TxBlockPool<skiq_tx_block_t>;
  std::unique_ptr<TxBlocks> tx_blocks;
  void initializeTxBuffers();
  std::shared_ptr<TxCompletion> tx_completion{std::make_shared<TxCompletion>(DEFAULT_NUM_BUFFERS)};
  std::mutex tx_mutex;
  FixtureCondition tx_cv;
  bool tx_start_finished{};
  int tx_start_status{};
  std::vector<std::vector<std::int16_t>> transmitted;
  bool complete_on_worker{};
  bool defer_completion{};
  std::vector<passedStruct *> pending;
  std::vector<int> send_results;
  std::size_t attempts{};
  bool reject_notifies{true};
  std::function<void()> on_submit;

  SoapySidekiq()
  {
      initializeTxBuffers();
      tx_completion->fixture_owner = this;
  }

    ~SoapySidekiq()
    {
        if (_tx_streaming_thread.joinable())
            _tx_streaming_thread.join();
        for (auto *context : pending)
            delete context;
    }

    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    StreamHandle *findStreamHandle(SoapySDR::Stream *) const noexcept;
    std::timed_mutex tx_status_mutex;
    std::uint32_t tx_underruns{};
    int readStreamStatus(SoapySDR::Stream *, std::size_t &, int &, long long &, long);
    void tx_streaming_start(skiq_tx_hdl_t) noexcept;
    int writeStream(SoapySDR::Stream *, const void *const *, std::size_t,
                    int &, long long, long);
    static void completeTx(const std::shared_ptr<TxCompletion> &, std::uint32_t);
    void tx_complete(int32_t, skiq_tx_block_t *, uint32_t index)
    {
        completeTx(tx_completion, index);
    }
    static void tx_complete_callback(int32_t, skiq_tx_block_t *, void *) noexcept;
    void closeStream(SoapySDR::Stream *);
    void stopTxWrites();
    int stopTxStream(bool);
    int deactivateStream(SoapySDR::Stream *, int, long long);
    int activateStream(SoapySDR::Stream *, int, long long, std::size_t);
    int activateStreamImpl(SoapySDR::Stream *, int, long long, std::size_t);
};

#include "TxBufferInitialization.inc"
#include "TxCloseMethod.inc"
#include "TxCompletionMethod.inc"
#include "TxCompletionCallback.inc"

/** Capture each accepted packet and complete its ownership synchronously. */
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *block, passedStruct *context)
{
    auto &device = *context->completion->fixture_owner;
    if (device.on_submit)
        device.on_submit();
    const auto attempt = device.attempts++;
    const int status = attempt < device.send_results.size() ? device.send_results[attempt] : 0;
    if (status != 0)
    {
        // Rejected sends never acquire this context. Optionally model an
        // earlier queued packet completing during this SDK call.
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL && device.reject_notifies)
        {
            const auto prior_index = (context->txIndex + 1) % device.tx_buffer_count;
            {
                std::lock_guard<std::mutex> lock(device.tx_completion->mutex);
                device.tx_completion->slots[prior_index] = 1;
            }
            device.tx_complete(0, device.tx_blocks->block(prior_index).get(), prior_index);
        }
        return status;
    }
    device.transmitted.emplace_back(block->data, block->data + 2 * device.current_tx_block_size);
    if (device.defer_completion)
    {
        device.pending.push_back(context);
        return 0;
    }
    if (device.complete_on_worker)
    {
        // A different SDK thread must be able to acquire the status lock while
        // transmit is in progress. try_to_lock makes a bad lock scope fail
        // this test instead of deadlocking the suite.
        bool completed = false;
        std::thread completion(
            [&]
            {
                std::unique_lock<std::mutex> lock(device.tx_completion->mutex, std::try_to_lock);
                if (lock.owns_lock())
                {
                    lock.unlock();
                    device.tx_complete_callback(0, block, context);
                    completed = true;
                }
            });
        completion.join();
        REQUIRE(completed);
    }
    else
    {
        device.tx_complete_callback(0, block, context);
    }
    return 0;
}

#include "StreamHandleLookup.inc"
#include "TxActivationMethods.inc"
#include "StreamStatusMethod.inc"

static soapy_sidekiq::StreamDirection streamDirection(int direction)
{
    if (direction == SOAPY_SDR_RX) return soapy_sidekiq::StreamDirection::rx;
    if (direction == SOAPY_SDR_TX) return soapy_sidekiq::StreamDirection::tx;
    throw std::invalid_argument("invalid stream direction");
}
#include "NativeStreamFormat.inc"
#include "ChannelValidationMethods.inc"

// Generated directly from Streaming.cpp by CMake, including the input advance.
#include "TxWriteMethod.inc"
#include "TxStopWritesMethod.inc"
#include "TxDeactivateMethod.inc"
#include "TxStartMethod.inc"

TEST_CASE("TX completion can release status from another thread during submission")
{
    SoapySidekiq device;
    device.complete_on_worker = true;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
}

TEST_CASE("failed PPS startup rejects every write without submitting packets")
{
    SoapySidekiq device;
    device.first_transmit = true;
    device.tx_start_finished = true;
    device.tx_start_status = -5;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (int retry = 0; retry < 3; ++retry)
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                      buffers, device.current_tx_block_size, flags, 0, 0),
                   SOAPY_SDR_STREAM_ERROR);
        REQUIRE(device.first_transmit);
        REQUIRE(device.transmitted.empty());
    }
}

TEST_CASE("successful PPS startup opens the write gate for subsequent packets")
{
    SoapySidekiq device;
    device.first_transmit = true;
    device.tx_start_finished = true;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (int write = 0; write < 2; ++write)
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                      buffers, device.current_tx_block_size, flags, 0, 0),
                   static_cast<int>(device.current_tx_block_size));
        REQUIRE(!device.first_transmit);
    }
    REQUIRE_EQ(device.transmitted.size(), std::size_t{2});
}

TEST_CASE("CF32 TX transmits every sample from three distinct input blocks")
{
    SoapySidekiq device;
    const std::size_t words_per_block = 2 * device.current_tx_block_size;
    std::vector<float> input(3 * words_per_block);
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        // Alternate signed I/Q components within the normalized range.
        input[i] = static_cast<float>(i / words_per_block + 1) * 0.125f +
                   static_cast<float>(i % 8) * 0.03125f;
        if (i % 2 != 0) input[i] = -input[i];
    }
    // Cover signed full scale, zero, and the specific negative review example.
    input[0] = -1.0f;
    input[1] = 1.0f;
    input[2] = 0.0f;
    input[3] = -0.25f;
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                  buffers, input.size() / 2, flags, 0, 0),
               static_cast<int>(input.size() / 2));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{3});
    for (std::size_t block = 0; block < device.transmitted.size(); ++block)
    {
        for (std::size_t word = 0; word < words_per_block; ++word)
        {
            REQUIRE_EQ(device.transmitted[block][word], static_cast<std::int16_t>(std::lround(
                input[block * words_per_block + word] * static_cast<float>(device.txMaxValue))));
        }
    }
}

TEST_CASE("CS16 TX preserves every signed sample across three input blocks")
{
    SoapySidekiq device;
    device.txUseShort = true;
    const std::size_t words_per_block = 2 * device.current_tx_block_size;
    std::vector<std::int16_t> input(3 * words_per_block);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<std::int16_t>(static_cast<int>(i) - 700);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                  buffers, input.size() / 2, flags, 0, 0),
               static_cast<int>(input.size() / 2));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{3});
    for (std::size_t block = 0; block < device.transmitted.size(); ++block)
    {
        const auto first = input.begin() + static_cast<std::ptrdiff_t>(block * words_per_block);
        REQUIRE(device.transmitted[block] == std::vector<std::int16_t>(
            first, first + static_cast<std::ptrdiff_t>(words_per_block)));
    }
}

TEST_CASE("TX host alignment determines the transmitted CF32 amplitude")
{
    using soapy_sidekiq::TxSampleAlignment;
    struct Layout
    {
        std::uint8_t bits;
        TxSampleAlignment alignment;
        std::int16_t expected_half;
        double expected_scale;
    };
    // Independent values from the SDK unpacked-sample layout, not the converter.
    const Layout layouts[]{
        {12, TxSampleAlignment::right_justified, 1024, 2047},
        {14, TxSampleAlignment::upper_bits, 16384, 32767},
        {16, TxSampleAlignment::right_justified, 16384, 32767}
    };
    for (const auto &layout : layouts)
    {
        SoapySidekiq device;
        device.txMaxValue = soapy_sidekiq::txSampleFullScale(layout.bits, layout.alignment);
        REQUIRE_EQ(device.txMaxValue, layout.expected_scale);
        const std::size_t words = 2 * device.current_tx_block_size;
        std::vector<float> input(3 * words);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = i % 2 == 0 ? 0.5f : -0.5f;
        const void *buffers[]{input.data()};
        int flags = 0;
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                      buffers, input.size() / 2, flags, 0, 0),
                   static_cast<int>(input.size() / 2));
        REQUIRE_EQ(device.transmitted.size(), std::size_t{3});
        for (const auto &block : device.transmitted)
            for (std::size_t i = 0; i < words; ++i)
                REQUIRE_EQ(block[i], i % 2 == 0 ? layout.expected_half : -layout.expected_half);
    }
}

TEST_CASE("native CS16 reporting uses the host layout rather than physical DAC width")
{
    using soapy_sidekiq::TxSampleAlignment;
    SoapySidekiq device;
    device.maxValue = soapy_sidekiq::sampleFullScale(16);
    device.txMaxValue = soapy_sidekiq::txSampleFullScale(14, TxSampleAlignment::upper_bits);
    double scale = 0;
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_RX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 32767.0);
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_TX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 32767.0);
    device.txMaxValue = soapy_sidekiq::txSampleFullScale(12, TxSampleAlignment::right_justified);
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_TX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 2047.0);
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_RX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 32767.0);
    REQUIRE_THROWS_AS(device.getNativeStreamFormat(99, 0, scale), std::invalid_argument);
}

TEST_CASE("TX never overwrites a busy SDK block before completion")
{
    SoapySidekiq device;
    device.txUseShort = true;
    device.tx_completion->slots[0] = 1;
    std::fill(std::begin(device.tx_blocks->block(0)->data),
              std::end(device.tx_blocks->block(0)->data), 123);
    std::vector<std::int16_t> input(2 * device.current_tx_block_size, 456);
    bool original_payload_preserved = false;
    wait_hook = [&]
    {
        original_payload_preserved = std::all_of(std::begin(device.tx_blocks->block(0)->data),
                                                 std::end(device.tx_blocks->block(0)->data),
                                                 [](auto sample) { return sample == 123; });
        device.tx_complete(0, device.tx_blocks->block(0).get(), 0);
    };
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 1000000),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE(original_payload_preserved);
    REQUIRE(device.transmitted[0] == input);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("queue-full TX retries release context and retain the input block")
{
    SoapySidekiq device;
    device.txUseShort = true;
    device.send_results = {SKIQ_TX_ASYNC_SEND_QUEUE_FULL, SKIQ_TX_ASYNC_SEND_QUEUE_FULL, 0};
    std::vector<std::int16_t> input(2 * device.current_tx_block_size, -789);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 1000000),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.attempts, std::size_t{3});
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
    REQUIRE(device.transmitted[0] == input);
    REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{1});
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("hard TX errors release reservation and context before a retry")
{
    SoapySidekiq device;
    device.send_results = {-5, 0};
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
    REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("CF32 conversion failure releases its reserved TX block and context")
{
    SoapySidekiq device;
    device.txMaxValue = 0;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("callback context allocation failure releases its reserved TX block")
{
    SoapySidekiq device;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    passedStruct::fail_allocation = true;
    soapy_sidekiq::ScopeExit restore([]() noexcept { passedStruct::fail_allocation = false; });
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("accepted TX contexts survive submission until out-of-order completion")
{
    SoapySidekiq device;
    device.defer_completion = true;
    std::vector<float> input(4 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  2 * device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(2 * device.current_tx_block_size));
    REQUIRE_EQ(device.pending.size(), std::size_t{2});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{2});
    REQUIRE_EQ(device.tx_completion->slots[0], 1);
    REQUIRE_EQ(device.tx_completion->slots[1], 1);
    for (auto it = device.pending.rbegin(); it != device.pending.rend(); ++it)
    {
        auto *context = *it;
        device.tx_complete(0, device.tx_blocks->block(context->txIndex).get(), context->txIndex);
        delete context;
    }
    device.pending.clear();
    REQUIRE_EQ(device.tx_completion->slots[0], 0);
    REQUIRE_EQ(device.tx_completion->slots[1], 0);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("pending PPS startup respects zero and finite write timeouts")
{
    SoapySidekiq device;
    device.first_transmit = true;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (long timeout : {0L, 1000L})
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      device.current_tx_block_size, flags, 0, timeout),
                   SOAPY_SDR_TIMEOUT);
        REQUIRE(device.first_transmit);
        REQUIRE(device.transmitted.empty());
        REQUIRE_EQ(device.tx_completion->slots[0], 0);
    }
    // A timeout must not close the gate permanently once the start completes.
    {
        std::lock_guard<std::mutex> lock(device.tx_mutex);
        device.tx_start_finished = true;
    }
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE(!device.first_transmit);
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
}

TEST_CASE("negative TX timeout fails without consuming the PPS write gate")
{
    SoapySidekiq device;
    device.first_transmit = true;
    const void *buffers[]{nullptr};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, -1),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE(device.first_transmit);
    REQUIRE(device.transmitted.empty());
}

TEST_CASE("PPS completion wakes a write with the maximum timeout safely")
{
    SoapySidekiq device;
    device.first_transmit = true;
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    std::thread completion(
        [&]
        {
            {
                std::lock_guard<std::mutex> lock(device.tx_mutex);
                device.tx_start_finished = true;
                device.tx_start_status = 0;
            }
            device.tx_cv.notify_all();
        });
    int flags = 0;
    // Always join before assertions; the fixture is borrowed by this thread.
    const int result = device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                          buffers, device.current_tx_block_size, flags, 0,
                                          std::numeric_limits<long>::max());
    completion.join();
    REQUIRE_EQ(result, static_cast<int>(device.current_tx_block_size));
    REQUIRE(!device.first_transmit);
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
}

TEST_CASE("busy TX blocks respect zero and finite timeouts without payload writes")
{
    SoapySidekiq device;
    device.tx_completion->slots[0] = 1;
    std::fill(std::begin(device.tx_blocks->block(0)->data),
              std::end(device.tx_blocks->block(0)->data), 123);
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (long timeout : {0L, 1000L})
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      device.current_tx_block_size, flags, 0, timeout),
                   SOAPY_SDR_TIMEOUT);
        REQUIRE_EQ(device.attempts, std::size_t{0});
        REQUIRE_EQ(device.tx_completion->slots[0], 1);
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
        REQUIRE(std::all_of(std::begin(device.tx_blocks->block(0)->data),
                            std::end(device.tx_blocks->block(0)->data),
                            [](auto sample) { return sample == 123; }));
    }
}

TEST_CASE("queue-full TX waits time out and leave submission resources retryable")
{
    SoapySidekiq device;
    device.reject_notifies = false;
    device.send_results = {SKIQ_TX_ASYNC_SEND_QUEUE_FULL, SKIQ_TX_ASYNC_SEND_QUEUE_FULL, 0};
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (long timeout : {0L, 1000L})
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      device.current_tx_block_size, flags, 0, timeout),
                   SOAPY_SDR_TIMEOUT);
        REQUIRE_EQ(device.tx_completion->slots[0], 0);
        REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{0});
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    }
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
}

TEST_CASE("TX back-pressure after a submitted prefix returns that sample count")
{
    SoapySidekiq device;
    device.txUseShort = true;
    device.reject_notifies = false;
    device.send_results = {0, SKIQ_TX_ASYNC_SEND_QUEUE_FULL};
    std::vector<std::int16_t> input(4 * device.current_tx_block_size, -123);
    std::fill(input.begin() + 2 * device.current_tx_block_size, input.end(), 456);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  2 * device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
    REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{1});
    REQUIRE_EQ(device.tx_completion->slots[1], 0);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    // The caller can resume from the returned prefix without repeating it.
    const void *remainder[]{input.data() + 2 * device.current_tx_block_size};
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), remainder,
                                  device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{2});
    REQUIRE(std::all_of(device.transmitted[0].begin(), device.transmitted[0].end(),
                        [](auto sample) { return sample == -123; }));
    REQUIRE(std::all_of(device.transmitted[1].begin(), device.transmitted[1].end(),
                        [](auto sample) { return sample == 456; }));
}

TEST_CASE("spurious TX wakeups cannot free a busy hardware block")
{
    SoapySidekiq device;
    device.tx_completion->slots[0] = 1;
    bool notified = false;
    wait_hook = [&] { notified = true; };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 1000),
               SOAPY_SDR_TIMEOUT);
    REQUIRE(notified);
    REQUIRE_EQ(device.tx_completion->slots[0], 1);
    REQUIRE_EQ(device.attempts, std::size_t{0});
}

TEST_CASE("PPS startup and TX buffer waits share one deadline")
{
    SoapySidekiq device;
    device.first_transmit = true;
    device.tx_completion->slots[0] = 1;
    wait_hook = [&]
    {
        std::lock_guard<std::mutex> lock(device.tx_mutex);
        device.tx_start_finished = true;
    };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 1000000),
               SOAPY_SDR_TIMEOUT);
    REQUIRE_EQ(device.tx_cv.deadlines.size(), std::size_t{1});
    REQUIRE_EQ(device.tx_completion->changed.deadlines.size(), std::size_t{1});
    REQUIRE(device.tx_cv.deadlines[0] == device.tx_completion->changed.deadlines[0]);
    REQUIRE_EQ(device.attempts, std::size_t{0});
}

TEST_CASE("TX buffer completion wakes the maximum timeout without overflow")
{
    SoapySidekiq device;
    device.tx_completion->slots[0] = 1;
    wait_hook = [&] { device.tx_complete(0, device.tx_blocks->block(0).get(), 0); };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0,
                                  std::numeric_limits<long>::max()),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{1});
}

TEST_CASE("TX rejects partial and unrepresentable sample counts before PPS waits")
{
    SoapySidekiq device;
    device.first_transmit = true;
    int flags = 0;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    for (const auto count : {std::size_t{1}, std::size_t{device.current_tx_block_size - 1},
                             static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1})
        REQUIRE_EQ(device.writeStream(stream, nullptr, count, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE(device.tx_cv.deadlines.empty());
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}
TEST_CASE("TX zero-length and null-buffer requests never submit or wait")
{
    SoapySidekiq device;
    device.first_transmit = true;
    int flags = 0;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    REQUIRE_EQ(device.writeStream(stream, nullptr, 0, flags, 0, 0), 0);
    REQUIRE_EQ(device.writeStream(stream, nullptr, device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    const void *buffers[]{nullptr};
    REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE(device.tx_cv.deadlines.empty());
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}
TEST_CASE("production CF32 writes clip exceptional values at each supported TX scale")
{
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<float> values{-infinity,
                                    -2.0f,
                                    -1.0f,
                                    -0.5f,
                                    -0.0f,
                                    0.0f,
                                    0.5f,
                                    1.0f,
                                    2.0f,
                                    infinity,
                                    std::numeric_limits<float>::quiet_NaN()};
    for (const int scale : {2047, 8191, 32767})
    {
        SoapySidekiq device;
        device.txMaxValue = scale;
        std::vector<float> input(2 * device.current_tx_block_size);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = values[i % values.size()];
        const std::vector<std::int16_t> expected{static_cast<std::int16_t>(-scale),
                                                 static_cast<std::int16_t>(-scale),
                                                 static_cast<std::int16_t>(-scale),
                                                 static_cast<std::int16_t>(-(scale + 1) / 2),
                                                 0,
                                                 0,
                                                 static_cast<std::int16_t>((scale + 1) / 2),
                                                 static_cast<std::int16_t>(scale),
                                                 static_cast<std::int16_t>(scale),
                                                 static_cast<std::int16_t>(scale),
                                                 0};
        const void *buffers[]{input.data()};
        int flags = 0;
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      device.current_tx_block_size, flags, 0, 0),
                   static_cast<int>(device.current_tx_block_size));
        for (std::size_t i = 0; i < input.size(); ++i)
            REQUIRE_EQ(device.transmitted[0][i], expected[i % expected.size()]);
    }
}
TEST_CASE("CF32 suffix retry after back-pressure preserves input stride and packet order")
{
    SoapySidekiq device;
    device.reject_notifies = false;
    device.send_results = {0, SKIQ_TX_ASYNC_SEND_QUEUE_FULL, 0};
    std::vector<float> input(4 * device.current_tx_block_size, -1.0f);
    std::fill(input.begin() + 2 * device.current_tx_block_size, input.end(), 1.0f);
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    const void *buffers[]{input.data()};
    int flags = 0;
    const int accepted =
        device.writeStream(stream, buffers, 2 * device.current_tx_block_size, flags, 0, 0);
    REQUIRE_EQ(accepted, static_cast<int>(device.current_tx_block_size));
    const void *suffix[]{input.data() + 2 * accepted};
    REQUIRE_EQ(device.writeStream(stream, suffix, device.current_tx_block_size, flags, 0, 0),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.transmitted.size(), std::size_t{2});
    REQUIRE(std::all_of(device.transmitted[0].begin(), device.transmitted[0].end(),
                        [](auto value) { return value == -32767; }));
    REQUIRE(std::all_of(device.transmitted[1].begin(), device.transmitted[1].end(),
                        [](auto value) { return value == 32767; }));
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX callback releases context after throwing diagnostics and publishes completion first")
{
    SoapySidekiq device;
    auto *context = new passedStruct;
    context->completion = device.tx_completion;
    context->txIndex = 0;
    // An unexpected prior status exercises the diagnostic path.
    device.tx_completion->slots[0] = 0;
    bool published = false, unlocked = false;
    log_hook = [&]
    {
        std::unique_lock<std::mutex> lock(device.tx_completion->mutex, std::try_to_lock);
        unlocked = lock.owns_lock();
        published =
            unlocked && device.tx_completion->slots[0] == 0 && device.tx_completion->completed == 1;
        throw std::runtime_error("injected log failure");
    };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    static_assert(noexcept(SoapySidekiq::tx_complete_callback(0, nullptr, nullptr)));
    SoapySidekiq::tx_complete_callback(0, device.tx_blocks->block(0).get(), context);
    REQUIRE(published);
    REQUIRE(unlocked);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX callback contains invalid-user diagnostics and frees ownerless contexts")
{
    log_hook = [] { throw std::runtime_error("injected log failure"); };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    SoapySidekiq::tx_complete_callback(0, nullptr, nullptr);
    SoapySidekiq::tx_complete_callback(0, nullptr, new passedStruct);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX callback rejects out-of-range indices without touching status storage")
{
    SoapySidekiq device;
    auto *context = new passedStruct;
    context->completion = device.tx_completion;
    context->txIndex = DEFAULT_NUM_BUFFERS;
    log_hook = [] { throw std::runtime_error("injected log failure"); };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    SoapySidekiq::tx_complete_callback(0, nullptr, context);
    REQUIRE_EQ(device.tx_completion->completed, std::uint64_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    for (const auto slot : device.tx_completion->slots)
        REQUIRE_EQ(slot, 0);
}

// A promise marks the actual condition-variable wait boundary. Deactivation
// runs on the calling thread and must drain the writer before returning.
static void checkCancelledWrite(bool pps, bool queue_full, bool partial)
{
    SoapySidekiq device;
    device.first_transmit = pps;
    device.txUseShort = true;
    device.reject_notifies = false;
    if (queue_full)
        device.send_results = partial ? std::vector<int>{0, SKIQ_TX_ASYNC_SEND_QUEUE_FULL}
                                      : std::vector<int>{SKIQ_TX_ASYNC_SEND_QUEUE_FULL};
    else if (!pps)
        device.tx_completion->slots[0] = 1;
    std::promise<void> waiting;
    auto entered = waiting.get_future();
    wait_hook = [&] { waiting.set_value(); };
    std::vector<std::int16_t> input(2 * device.current_tx_block_size * (partial ? 2 : 1), 17);
    const void *buffers[]{input.data()};
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    auto writer = std::async(std::launch::async,
                             [&]
                             {
                                 int flags = 0;
                                 return device.writeStream(stream, buffers, input.size() / 2, flags,
                                                           0, std::numeric_limits<long>::max());
                             });
    entered.wait();
    stop_status = 0;
    stop_calls = 0;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE_EQ(writer.get(),
               partial ? static_cast<int>(device.current_tx_block_size) : SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
    REQUIRE(device.tx_writes_stopping);
    REQUIRE(!device.tx_stream_active);
    REQUIRE_EQ(stop_calls, 1);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    const auto attempts = device.attempts;
    int flags = 0;
    REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.attempts, attempts);
}

TEST_CASE("deactivation cancels a writer waiting for PPS startup")
{
    checkCancelledWrite(true, false, false);
}
TEST_CASE("deactivation cancels a writer waiting for a busy TX buffer")
{
    checkCancelledWrite(false, false, false);
}
TEST_CASE("deactivation cancels a queue-full writer and releases rejected context")
{
    checkCancelledWrite(false, true, false);
}
TEST_CASE("deactivation returns the accepted prefix from a queue-full write")
{
    checkCancelledWrite(false, true, true);
}
TEST_CASE("failed SDK stop keeps stream ownership and closes write admission until retry")
{
    for (const int flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        stop_status = -EIO;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE(device.tx_stream_active);
        REQUIRE(device.active_tx_stream.get() == &device.handle);
        REQUIRE(device.tx_writes_stopping);
        stop_status = 0;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), 0);
        REQUIRE(!device.tx_stream_active);
    }
}

TEST_CASE("deactivation drains a submission before calling SDK stop")
{
    SoapySidekiq device;
    std::promise<void> submitted, release;
    auto submitting = submitted.get_future();
    auto allowed = release.get_future();
    device.on_submit = [&]
    {
        submitted.set_value();
        allowed.wait();
    };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    auto writer = std::async(std::launch::async,
                             [&]
                             {
                                 int flags = 0;
                                 return device.writeStream(
                                     stream, buffers, device.current_tx_block_size, flags, 0, 0);
                             });
    submitting.wait();
    stop_status = 0;
    stop_calls = 0;
    auto stopping =
        std::async(std::launch::async, [&] { return device.deactivateStream(stream, 0, 0); });
    bool admission_closed;
    {
        std::unique_lock<std::mutex> lock(device.tx_completion->mutex);
        admission_closed = device.tx_completion->changed.wait_until(
            lock, std::chrono::steady_clock::now() + std::chrono::seconds(2),
            [&] { return device.tx_writes_stopping; });
    }
    // Capture observations before releasing the submission, but assert only
    // after joining so a failed assertion cannot strand a waiting test thread.
    const bool drained_early =
        stopping.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    release.set_value();
    REQUIRE_EQ(writer.get(), static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(stopping.get(), 0);
    REQUIRE(admission_closed);
    REQUIRE(!drained_early);
    REQUIRE_EQ(stop_calls, 1);
    REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
}

TEST_CASE("unsupported TX write flags are rejected before waits and submissions")
{
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    device.first_transmit = true;
    device.tx_start_finished = false;
    for (const int requested : {1, 2, SOAPY_SDR_HAS_TIME, SOAPY_SDR_HAS_TIME | 2, -1})
    {
        int flags = requested;
        REQUIRE_EQ(device.writeStream(stream, nullptr, device.current_tx_block_size, flags,
                                      1000000000, 1000000),
                   SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(flags, requested);
    }
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE(device.tx_cv.deadlines.empty());
    REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
    REQUIRE(device.first_transmit);
}
TEST_CASE("TX stop rejects unsupported flags and times without closing write admission")
{
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    stop_calls = 0;
    for (const int flags : {1, SOAPY_SDR_HAS_TIME | 2, -1})
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 1000000000),
               SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(stop_calls, 0);
    REQUIRE(device.tx_stream_active);
    REQUIRE(!device.tx_writes_stopping);
}

TEST_CASE("TX APIs reject unowned tokens before writes status reads or shutdown")
{
    SoapySidekiq device;
    SoapySidekiq foreign;
    underrun_reads = 0;
    int flags = 123;
    long long timestamp = 456;
    std::size_t channels = 789;
    for (auto *invalid : {static_cast<SoapySDR::Stream *>(nullptr),
                          reinterpret_cast<SoapySDR::Stream *>(std::uintptr_t{1}),
                          reinterpret_cast<SoapySDR::Stream *>(&foreign.handle)})
    {
        REQUIRE_EQ(device.writeStream(invalid, nullptr, 252, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.readStreamStatus(invalid, channels, flags, timestamp, 0),
                   SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.deactivateStream(invalid, 0, 0), SOAPY_SDR_STREAM_ERROR);
    }
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE_EQ(underrun_reads, 0u);
    REQUIRE(device.tx_stream_active);
    REQUIRE_EQ(flags, 123);
    REQUIRE_EQ(timestamp, 456LL);
    REQUIRE_EQ(channels, std::size_t{789});
}
TEST_CASE("detached TX token is rejected and current token retains status access")
{
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    int flags = 0;
    long long timestamp = 0;
    std::size_t channels = 0;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, timestamp, 0), SOAPY_SDR_TIMEOUT);
    device.active_tx_stream = nullptr;
    REQUIRE_EQ(device.writeStream(stream, nullptr, 252, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, timestamp, 0),
               SOAPY_SDR_STREAM_ERROR);
}

namespace
{
/** Reset scripted status replies between independent production-method cases. */
void resetUnderruns()
{
    underrun_counts.clear();
    underrun_reads = 0;
    underrun_status = 0;
    underrun_hook = {};
    wait_hook = {};
}
} // namespace
TEST_CASE("TX status reports one underrun event with stream-relative metadata")
{
    resetUnderruns();
    SoapySidekiq device;
    device.handle.tx_handle = 7;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    underrun_counts = {2, 2};
    std::size_t channels = 99;
    int flags = 99;
    long long time = 99;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_UNDERFLOW);
    REQUIRE_EQ(channels, std::size_t{1});
    REQUIRE_EQ(flags, 0);
    REQUIRE_EQ(time, 0LL);
    REQUIRE_EQ(underrun_handle, 7u);
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_TIMEOUT);
    REQUIRE_EQ(channels, std::size_t{0});
}
TEST_CASE("TX status honors a finite timeout without a busy polling loop")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 2000), SOAPY_SDR_TIMEOUT);
    REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::microseconds(2000));
    REQUIRE(!device.tx_completion->changed.deadlines.empty());
    REQUIRE(underrun_reads < 20);
}
TEST_CASE("TX status observes a later event within a shared deadline")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    underrun_counts = {0, 1};
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 1000000),
               SOAPY_SDR_UNDERFLOW);
    REQUIRE_EQ(underrun_reads, std::size_t{2});
}
TEST_CASE("TX status rejects invalid timeout and reports SDK read failures as status codes")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, -1), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(underrun_reads, std::size_t{0});
    underrun_status = -5;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
}
TEST_CASE("TX status waits for PPS startup and rejects failed or stopped sessions")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    device.first_transmit = true;
    device.tx_start_finished = false;
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_TIMEOUT);
    device.tx_start_finished = true;
    device.tx_start_status = -5;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
    device.first_transmit = false;
    device.tx_writes_stopping = true;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(underrun_reads, std::size_t{0});
}
TEST_CASE("TX status is interrupted when shutdown closes admission")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    wait_hook = [&]
    {
        std::scoped_lock lock(device.tx_mutex, device.tx_completion->mutex);
        device.tx_writes_stopping = true;
    };
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 1000000),
               SOAPY_SDR_STREAM_ERROR);
}
TEST_CASE("TX counter decreases rebase the cache for subsequent underruns")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    device.tx_underruns = 10;
    underrun_counts = {0, 1};
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_TIMEOUT);
    REQUIRE_EQ(device.readStreamStatus(stream, channels, flags, time, 0), SOAPY_SDR_UNDERFLOW);
}
TEST_CASE("successful PPS startup resets the host underrun baseline")
{
    resetUnderruns();
    SoapySidekiq device;
    device.tx_underruns = 10;
    pps_start_status = 0;
    device.tx_streaming_start(7);
    REQUIRE_EQ(device.tx_underruns, std::uint32_t{0});
    REQUIRE(device.tx_start_finished);
    REQUIRE_EQ(device.tx_start_status, 0);
    device.tx_underruns = 10;
    pps_start_status = -5;
    device.tx_streaming_start(7);
    REQUIRE_EQ(device.tx_underruns, std::uint32_t{10});
    REQUIRE_EQ(device.tx_start_status, -5);
}
TEST_CASE("TX status saturates very large timeout budgets without overflow")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    underrun_counts = {1};
    std::size_t channels = 0;
    int flags = 0;
    long long time = 0;
    REQUIRE_EQ(
        device.readStreamStatus(stream, channels, flags, time, std::numeric_limits<long>::max()),
        SOAPY_SDR_UNDERFLOW);
}

TEST_CASE("concurrent TX pollers consume a counter increase only once")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    underrun_counts = {1};
    const auto poll = [&]
    {
        std::size_t channels = 0;
        int flags = 0;
        long long time = 0;
        return device.readStreamStatus(stream, channels, flags, time, 0);
    };
    auto first = std::async(std::launch::async, poll);
    auto second = std::async(std::launch::async, poll);
    const int a = first.get(), b = second.get();
    REQUIRE((a == SOAPY_SDR_UNDERFLOW && b == SOAPY_SDR_TIMEOUT) ||
            (b == SOAPY_SDR_UNDERFLOW && a == SOAPY_SDR_TIMEOUT));
}

TEST_CASE("TX shutdown drains an admitted status SDK read before returning")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::promise<void> entered, release, stop_entered;
    auto released = release.get_future().share();
    auto entered_future = entered.get_future();
    bool first_read = true;
    underrun_hook = [&]
    {
        if (first_read)
        {
            first_read = false;
            entered.set_value();
            released.wait();
        }
    };
    auto poll =
        std::async(std::launch::async,
                   [&]
                   {
                       std::size_t channels = 0;
                       int flags = 0;
                       long long time = 0;
                       return device.readStreamStatus(stream, channels, flags, time, 1000000);
                   });
    const bool reached =
        entered_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!reached)
        release.set_value();
    REQUIRE(reached);
    auto stop = std::async(std::launch::async,
                           [&]
                           {
                               stop_entered.set_value();
                               device.stopTxWrites();
                           });
    stop_entered.get_future().wait();
    const bool draining =
        stop.wait_for(std::chrono::milliseconds(2)) == std::future_status::timeout;
    release.set_value();
    const auto result = poll.get();
    stop.get();
    underrun_hook = {};
    REQUIRE(draining);
    REQUIRE_EQ(result, SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.active_tx_status_readers, std::size_t{0});
}

TEST_CASE("TX status timeout includes waiting for another counter poller")
{
    resetUnderruns();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::unique_lock<std::timed_mutex> held(device.tx_status_mutex);
    auto poll = std::async(std::launch::async,
                           [&]
                           {
                               std::size_t channels = 0;
                               int flags = 0;
                               long long time = 0;
                               return device.readStreamStatus(stream, channels, flags, time, 2000);
                           });
    const bool ready = poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    held.unlock();
    const auto result = poll.get();
    REQUIRE(ready);
    REQUIRE_EQ(result, SOAPY_SDR_TIMEOUT);
    REQUIRE_EQ(underrun_reads, std::size_t{0});
}

TEST_CASE("hard TX submission failures preserve accepted prefixes and permit exact suffix retry")
{
    for (const bool deferred : {false, true})
        for (const std::size_t accepted : {std::size_t{1}, std::size_t{2}})
        {
            SoapySidekiq device;
            device.txUseShort = true;
            device.defer_completion = deferred;
            device.send_results.assign(accepted, 0);
            device.send_results.push_back(-EIO);
            const std::size_t block = device.current_tx_block_size;
            std::vector<std::int16_t> input(6 * block);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<std::int16_t>(static_cast<int>(i) - 700);
            const void *buffers[]{input.data()};
            auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
            int flags = 0;
            const int prefix = device.writeStream(stream, buffers, 3 * block, flags, 0, 0);
            REQUIRE_EQ(prefix, static_cast<int>(accepted * block));
            REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
            REQUIRE_EQ(device.currTXBuffIndex, static_cast<std::uint32_t>(accepted));
            REQUIRE_EQ(device.tx_completion->slots[accepted], 0);
            REQUIRE_EQ(passedStruct::live.load(), deferred ? accepted : std::size_t{0});
            const void *suffix[]{input.data() + 2 * prefix};
            REQUIRE_EQ(device.writeStream(stream, suffix, 3 * block - prefix, flags, 0, 0),
                       static_cast<int>(3 * block - prefix));
            REQUIRE_EQ(device.transmitted.size(), std::size_t{3});
            for (std::size_t packet = 0; packet < 3; ++packet)
            {
                const auto first = input.begin() + static_cast<std::ptrdiff_t>(2 * block * packet);
                REQUIRE(device.transmitted[packet] ==
                        std::vector<std::int16_t>(first, first + 2 * block));
            }
            REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
            REQUIRE_EQ(passedStruct::live.load(), deferred ? std::size_t{3} : std::size_t{0});
        }
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX preparation failures preserve prior submissions and release unsent reservations")
{
    for (const bool allocation : {false, true})
        for (const bool deferred : {false, true})
        {
            SoapySidekiq device;
            device.defer_completion = deferred;
            const std::size_t block = device.current_tx_block_size;
            std::vector<float> input(4 * block, 0.25f);
            const void *buffers[]{input.data()};
            auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
            int flags = 0;
            device.on_submit = [&]
            {
                if (allocation)
                    passedStruct::fail_allocation = true;
                else
                    device.txMaxValue = 0;
            };
            soapy_sidekiq::ScopeExit restore([]() noexcept
                                             { passedStruct::fail_allocation = false; });
            REQUIRE_EQ(device.writeStream(stream, buffers, 2 * block, flags, 0, 0),
                       static_cast<int>(block));
            REQUIRE_EQ(device.tx_completion->slots[1], 0);
            REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
            REQUIRE_EQ(device.attempts, std::size_t{1});
            REQUIRE_EQ(passedStruct::live.load(), deferred ? std::size_t{1} : std::size_t{0});
            passedStruct::fail_allocation = false;
            device.txMaxValue = 32767;
            device.on_submit = {};
            const void *suffix[]{input.data() + 2 * block};
            REQUIRE_EQ(device.writeStream(stream, suffix, block, flags, 0, 0),
                       static_cast<int>(block));
            REQUIRE_EQ(device.transmitted.size(), std::size_t{2});
        }
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX activation SDK failures return stream errors and allow a clean retry")
{
    for (std::size_t failure = 1; failure <= 6; ++failure)
    {
        SoapySidekiq device;
        device.tx_stream_active = false;
        device.tx_writes_stopping = true;
        device.handle.tx_handle = 7;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        activation_calls.clear();
        activation_fail_at = failure;
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(activation_calls.size(), failure);
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.tx_writes_stopping);
        REQUIRE(!device._tx_streaming_thread.joinable());
        activation_calls.clear();
        activation_fail_at = 0;
        device.tx_underruns = 99;
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        REQUIRE(device.tx_stream_active);
        REQUIRE(!device.tx_writes_stopping);
        REQUIRE_EQ(device.tx_underruns, 0u);
        REQUIRE_EQ(activation_handle, 7u);
    }
}

TEST_CASE("repeated TX activation rejects requests before SDK configuration")
{
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    activation_calls.clear();
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE(activation_calls.empty());
}

TEST_CASE("PPS TX activation cannot replace a joinable startup thread and can restart after stop")
{
    SoapySidekiq device;
    device.tx_stream_active = false;
    device.tx_writes_stopping = true;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    activation_calls.clear();
    activation_fail_at = 0;
    pps_start_status = -EIO;
    stop_status = 0;
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
    REQUIRE(device._tx_streaming_thread.joinable());
    const auto configured = activation_calls.size();
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(activation_calls.size(), configured);
    // The failed startup still owns its thread/stream until deactivation joins it.
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE(!device._tx_streaming_thread.joinable());
    REQUIRE(!device.tx_stream_active);
    pps_start_status = 0;
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
    REQUIRE(device.tx_stream_active);
    REQUIRE(!device.first_transmit);
}

TEST_CASE("unsupported TX activation options leave SDK configuration and startup untouched")
{
    SoapySidekiq device;
    device.tx_stream_active = false;
    device.tx_writes_stopping = true;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    activation_calls.clear();
    for (const int flags : {8, 16, -1, SOAPY_SDR_HAS_TIME | 8})
        REQUIRE_EQ(device.activateStream(stream, flags, 0, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 1), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 1, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE(activation_calls.empty());
    REQUIRE(!device.tx_stream_active);
    REQUIRE(device.tx_writes_stopping);
    REQUIRE(!device._tx_streaming_thread.joinable());
}

TEST_CASE("already stopped TX hardware deactivates successfully for immediate and PPS stops")
{
    for (const int flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        stop_status = -ENODEV;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), 0);
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.active_tx_stream.get() == &device.handle);
        REQUIRE(device.tx_writes_stopping);
    }
    stop_status = 0;
}

TEST_CASE(
    "accepted completion state survives device destruction and releases with the last context")
{
    auto device = std::make_unique<SoapySidekiq>();
    device->txUseShort = true;
    device->defer_completion = true;
    std::vector<std::int16_t> input(2 * device->current_tx_block_size, 17);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device->writeStream(reinterpret_cast<SoapySDR::Stream *>(&device->handle), buffers,
                                   device->current_tx_block_size, flags, 0, 100000),
               static_cast<int>(device->current_tx_block_size));
    REQUIRE_EQ(device->pending.size(), std::size_t{1});
    auto *context = device->pending.back();
    device->pending.clear(); // The simulated SDK retains this accepted context.
    const std::weak_ptr<TxCompletion> retained = device->tx_completion;
    const std::weak_ptr<skiq_tx_block_t> retained_block = context->block;
    device.reset();
    REQUIRE(!retained_block.expired());
    REQUIRE_EQ(context->block->data[0], 17);
    REQUIRE(!retained.expired());
    auto state = retained.lock();
    REQUIRE(state != nullptr);
    SoapySidekiq::tx_complete_callback(0, nullptr, context);
    REQUIRE_EQ(state->completed, std::uint64_t{1});
    REQUIRE_EQ(state->slots[0], 0);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    state.reset();
    REQUIRE(retained.expired());
    REQUIRE(retained_block.expired());
}

TEST_CASE("old stream completions cannot release a new stream reservation")
{
    SoapySidekiq device;
    auto *context = new passedStruct;
    context->completion = device.tx_completion;
    context->txIndex = 0;
    const auto old = context->completion;
    old->slots[0] = 1;
    device.tx_completion = std::make_shared<TxCompletion>(DEFAULT_NUM_BUFFERS);
    device.tx_completion->slots[0] = 1;
    SoapySidekiq::tx_complete_callback(0, nullptr, context);
    REQUIRE_EQ(old->slots[0], 0);
    REQUIRE_EQ(old->completed, std::uint64_t{1});
    REQUIRE_EQ(device.tx_completion->slots[0], 1);
    REQUIRE_EQ(device.tx_completion->completed, std::uint64_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("TX block allocation failure unwinds its prefix without replacing published owners")
{
    for (std::size_t prefix = 0; prefix < DEFAULT_NUM_BUFFERS; ++prefix)
    {
        SoapySidekiq device;
        const auto *pool = device.tx_blocks.get();
        const auto completion = device.tx_completion;
        const auto live = skiq_tx_block_t::live.load();
        device.currTXBuffIndex = 2;
        fail_block_allocation = block_allocations + prefix;
        soapy_sidekiq::ScopeExit restore(
            []() noexcept { fail_block_allocation = std::numeric_limits<std::size_t>::max(); });
        REQUIRE_THROWS_AS(device.initializeTxBuffers(), std::bad_alloc);
        REQUIRE_EQ(skiq_tx_block_t::live.load(), live);
        REQUIRE(device.tx_blocks.get() == pool);
        REQUIRE(device.tx_completion == completion);
        REQUIRE_EQ(device.currTXBuffIndex, 2u);
    }
}

TEST_CASE("TX block owners release unused storage when the device is destroyed")
{
    const auto baseline = skiq_tx_block_t::live.load();
    {
        SoapySidekiq device;
        REQUIRE_EQ(skiq_tx_block_t::live.load(), baseline + DEFAULT_NUM_BUFFERS);
    }
    REQUIRE_EQ(skiq_tx_block_t::live.load(), baseline);
}

TEST_CASE("closing TX retains accepted blocks until completion and isolates a replacement pool")
{
    const auto baseline = skiq_tx_block_t::live.load();
    SoapySidekiq device;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(device.active_tx_stream.get());
    device.txUseShort = true;
    device.defer_completion = true;
    std::vector<std::int16_t> input(2 * device.current_tx_block_size, 29);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 100000),
               static_cast<int>(device.current_tx_block_size));
    REQUIRE_EQ(device.pending.size(), std::size_t{1});
    std::unique_ptr<passedStruct> accepted(device.pending.back());
    device.pending.clear();
    const std::weak_ptr<skiq_tx_block_t> retained = accepted->block;
    stop_status = 0;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    device.closeStream(stream);
    REQUIRE(device.tx_blocks == nullptr);
    REQUIRE(device.active_tx_stream == nullptr);
    REQUIRE_EQ(skiq_tx_block_t::live.load(), baseline + 1);
    REQUIRE_EQ(accepted->block->data[0], 29);
    device.initializeTxBuffers();
    device.tx_completion->fixture_owner = &device;
    device.tx_completion->slots[0] = 1;
    auto *block = accepted->block.get();
    SoapySidekiq::tx_complete_callback(0, block, accepted.release());
    REQUIRE(retained.expired());
    REQUIRE_EQ(skiq_tx_block_t::live.load(), baseline + DEFAULT_NUM_BUFFERS);
    REQUIRE_EQ(device.tx_completion->slots[0], 1);
    REQUIRE_EQ(device.tx_completion->completed, std::uint64_t{0});
}

TEST_CASE("invalid TX pool dimensions fail before allocator access")
{
    const auto before = block_allocations;
    using Pool = soapy_sidekiq::TxBlockPool<skiq_tx_block_t>;
    REQUIRE_THROWS_AS((Pool(0, 252, skiq_tx_block_allocate, skiq_tx_block_free)),
                      std::invalid_argument);
    REQUIRE_THROWS_AS((Pool(3, 0, skiq_tx_block_allocate, skiq_tx_block_free)),
                      std::invalid_argument);
    REQUIRE_EQ(block_allocations, before);
}

TEST_CASE("throwing diagnostics after immediate TX start retain ownership for deactivation")
{
    SoapySidekiq device;
    device.tx_stream_active = false;
    device.tx_writes_stopping = true;
    activation_calls.clear();
    activation_fail_at = 0;
    bool failed_log = false;
    log_hook = [&]
    {
        if (!failed_log && !activation_calls.empty() && activation_calls.back() == "start")
        {
            failed_log = true;
            throw std::runtime_error("injected post-start diagnostic failure");
        }
    };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE(failed_log);
    REQUIRE(device.tx_stream_active);
    REQUIRE(device.tx_writes_stopping);
    const auto before = stop_calls;
    stop_status = 0;
    REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
    REQUIRE_EQ(stop_calls, before + 1);
    REQUIRE(!device.tx_stream_active);
}

TEST_CASE("configured small TX pools wrap their own ring across synchronous multi-block writes")
{
    for (const std::uint32_t count : {1u, 2u})
    {
        SoapySidekiq device;
        device.tx_buffer_count = count;
        device.initializeTxBuffers();
        device.tx_completion->fixture_owner = &device;
        device.txUseShort = true;
        REQUIRE_EQ(device.tx_completion->slots.size(), static_cast<std::size_t>(count));
        std::vector<std::int16_t> input(12 * device.current_tx_block_size);
        for (std::size_t block = 0; block < 6; ++block)
            std::fill_n(input.begin() +
                            static_cast<std::ptrdiff_t>(2 * block * device.current_tx_block_size),
                        2 * device.current_tx_block_size, static_cast<std::int16_t>(block + 1));
        const void *buffers[]{input.data()};
        int flags = 0;
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      6 * device.current_tx_block_size, flags, 0, 100000),
                   static_cast<int>(6 * device.current_tx_block_size));
        REQUIRE_EQ(device.currTXBuffIndex, 6u % count);
        REQUIRE_EQ(device.transmitted.size(), std::size_t{6});
        for (std::size_t block = 0; block < 6; ++block)
            REQUIRE_EQ(device.transmitted[block].front(), static_cast<std::int16_t>(block + 1));
    }
}

TEST_CASE("small TX pools retain accepted prefixes under back-pressure and retry the suffix")
{
    for (const std::uint32_t count : {1u, 2u})
    {
        SoapySidekiq device;
        device.tx_buffer_count = count;
        device.initializeTxBuffers();
        device.tx_completion->fixture_owner = &device;
        device.txUseShort = true;
        device.defer_completion = true;
        std::vector<std::int16_t> input(2 * (count + 1) * device.current_tx_block_size, 41);
        const void *buffers[]{input.data()};
        int flags = 0;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        const auto accepted = device.writeStream(
            stream, buffers, (count + 1) * device.current_tx_block_size, flags, 0, 0);
        REQUIRE_EQ(accepted, static_cast<int>(count * device.current_tx_block_size));
        REQUIRE_EQ(device.pending.size(), static_cast<std::size_t>(count));
        for (auto *context : device.pending)
        {
            auto *block = context->block.get();
            REQUIRE_EQ(block->data[0], 41);
            device.tx_complete_callback(0, block, context);
        }
        device.pending.clear();
        device.defer_completion = false;
        const void *suffix[]{input.data() + 2 * accepted};
        REQUIRE_EQ(
            device.writeStream(stream, suffix, device.current_tx_block_size, flags, 0, 100000),
            static_cast<int>(device.current_tx_block_size));
        REQUIRE_EQ(device.tx_completion->completed, static_cast<std::uint64_t>(count + 1));
    }
}

TEST_CASE(
    "actual PPS activation gates writes until the worker publishes its result and resets on retry")
{
    using namespace std::chrono_literals;
    for (const int start_status : {0, -EIO})
    {
        SoapySidekiq device;
        device.tx_stream_active = false;
        device.tx_writes_stopping = true;
        device.tx_underruns = 99;
        activation_calls.clear();
        activation_fail_at = 0;
        pps_start_status = start_status;
        stop_status = 0;
        std::promise<void> entered, release, writer_waiting;
        std::future<int> writing;
        std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
        const void *buffers[]{input.data()};
        const auto released = release.get_future().share();
        bool edge_released = false;
        // Release the fake edge and join before destroying captured promises,
        // including when an assertion fails while the SDK call is held open.
        soapy_sidekiq::ScopeExit cleanup(
            [&]() noexcept
            {
                if (!edge_released)
                    release.set_value();
                if (writing.valid())
                    writing.wait();
                if (device._tx_streaming_thread.joinable())
                    device._tx_streaming_thread.join();
                pps_start_hook = {};
                wait_hook = {};
            });
        pps_start_hook = [&]
        {
            entered.set_value();
            released.wait();
        };
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
        REQUIRE(entered.get_future().wait_for(1s) == std::future_status::ready);
        int flags = 0;
        REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 0),
                   SOAPY_SDR_TIMEOUT);
        REQUIRE_EQ(device.attempts, std::size_t{0});
        {
            std::lock_guard<std::mutex> lock(device.tx_mutex);
            REQUIRE(!device.tx_start_finished);
        }
        wait_hook = [&] { writer_waiting.set_value(); };
        writing =
            std::async(std::launch::async,
                       [&]
                       {
                           int write_flags = 0;
                           return device.writeStream(stream, buffers, device.current_tx_block_size,
                                                     write_flags, 0, 1000000);
                       });
        REQUIRE(writer_waiting.get_future().wait_for(1s) == std::future_status::ready);
        release.set_value();
        edge_released = true;
        REQUIRE(writing.wait_for(500ms) == std::future_status::ready);
        REQUIRE_EQ(writing.get(), start_status == 0 ? static_cast<int>(device.current_tx_block_size)
                                                    : SOAPY_SDR_STREAM_ERROR);
        device._tx_streaming_thread.join();
        REQUIRE(device.tx_start_finished);
        REQUIRE_EQ(device.tx_start_status, start_status);
        REQUIRE_EQ(device.tx_underruns, start_status == 0 ? 0u : 99u);
        REQUIRE_EQ(device.attempts, start_status == 0 ? std::size_t{1} : std::size_t{0});
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        REQUIRE(!device.tx_stream_active);
        REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
        activation_calls.clear();
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 0), 0);
        REQUIRE(!device.first_transmit);
        const auto before = device.attempts;
        REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 0),
                   static_cast<int>(device.current_tx_block_size));
        REQUIRE_EQ(device.attempts, before + 1);
        REQUIRE_EQ(device.deactivateStream(stream, 0, 0), 0);
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    }
}

TEST_CASE("deactivation cancels PPS-gated writes but joins pending SDK startup before stopping TX")
{
    using namespace std::chrono_literals;
    for (const int stop_flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        device.tx_stream_active = false;
        device.tx_writes_stopping = true;
        activation_calls.clear();
        activation_fail_at = 0;
        pps_start_status = 0;
        stop_status = 0;
        std::promise<void> entered, release, writer_waiting;
        const auto released = release.get_future().share();
        bool edge_released = false;
        std::atomic<bool> stopped{false};
        std::future<int> writing, stopping;
        // Futures must finish before captured device/input state leaves scope.
        std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
        const void *buffers[]{input.data()};
        soapy_sidekiq::ScopeExit cleanup(
            [&]() noexcept
            {
                if (!edge_released)
                    release.set_value();
                if (writing.valid())
                    writing.wait();
                if (stopping.valid())
                    stopping.wait();
                if (device._tx_streaming_thread.joinable())
                    device._tx_streaming_thread.join();
                pps_start_hook = {};
                stop_hook = {};
                wait_hook = {};
            });
        pps_start_hook = [&]
        {
            entered.set_value();
            released.wait();
        };
        stop_hook = [&] { stopped.store(true); };
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 0, 0), 0);
        REQUIRE(entered.get_future().wait_for(1s) == std::future_status::ready);
        auto *const owned_pool = device.tx_blocks.get();
        REQUIRE_THROWS_AS(device.closeStream(stream), std::runtime_error);
        REQUIRE(device.active_tx_stream.get() == &device.handle);
        REQUIRE(device.tx_blocks.get() == owned_pool);
        wait_hook = [&] { writer_waiting.set_value(); };
        writing =
            std::async(std::launch::async,
                       [&]
                       {
                           int flags = 0;
                           return device.writeStream(stream, buffers, device.current_tx_block_size,
                                                     flags, 0, 1000000);
                       });
        REQUIRE(writer_waiting.get_future().wait_for(1s) == std::future_status::ready);
        stopping = std::async(std::launch::async,
                              [&] { return device.deactivateStream(stream, stop_flags, 0); });
        REQUIRE(writing.wait_for(1s) == std::future_status::ready);
        REQUIRE_EQ(writing.get(), SOAPY_SDR_STREAM_ERROR);
        // The real SDK PPS call has no cancellation API. Verify the lifetime
        // ordering while held, then explicitly deliver the edge for cleanup.
        REQUIRE(stopping.wait_for(20ms) == std::future_status::timeout);
        REQUIRE(!stopped.load());
        REQUIRE_EQ(device.attempts, std::size_t{0});
        REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
        release.set_value();
        edge_released = true;
        REQUIRE_EQ(stopping.get(), 0);
        REQUIRE(stopped.load());
        REQUIRE(!device._tx_streaming_thread.joinable());
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.tx_writes_stopping);
        REQUIRE(device.active_tx_stream.get() == &device.handle);
    }
}

TEST_CASE("writes before first activation reject admission without waiting or submitting")
{
    SoapySidekiq device;
    // Match the device's constructed/setup state: a token exists, but the
    // activation path has not opened admission or completed a startup gate.
    device.tx_stream_active = false;
    device.tx_writes_stopping = true;
    device.first_transmit = true;
    device.tx_start_finished = false;
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (const std::size_t count : {std::size_t{0}, std::size_t{device.current_tx_block_size}})
        REQUIRE_EQ(device.writeStream(stream, buffers, count, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE(device.transmitted.empty());
    REQUIRE(device.tx_cv.deadlines.empty());
    REQUIRE(device.tx_completion->changed.deadlines.empty());
    REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("immediate and PPS deactivation keep empty and nonempty writes outside admission")
{
    for (const int stop_flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        stop_status = 0;
        stop_calls = 0;
        REQUIRE_EQ(device.deactivateStream(stream, stop_flags, 0), 0);
        REQUIRE_EQ(stop_calls, 1);
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.tx_writes_stopping);
        REQUIRE(device.active_tx_stream.get() == &device.handle);
        const auto prior_waits = device.tx_completion->changed.deadlines.size();
        std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
        const void *buffers[]{input.data()};
        int flags = 0;
        for (const std::size_t count : {std::size_t{0}, std::size_t{device.current_tx_block_size}})
            REQUIRE_EQ(device.writeStream(stream, buffers, count, flags, 0, 0),
                       SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device.tx_completion->changed.deadlines.size(), prior_waits);
        REQUIRE(device.tx_cv.deadlines.empty());
        REQUIRE_EQ(device.active_tx_writers, std::size_t{0});
        REQUIRE_EQ(device.attempts, std::size_t{0});
        REQUIRE(device.transmitted.empty());
        REQUIRE_EQ(device.currTXBuffIndex, std::uint32_t{0});
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    }
}

TEST_CASE("PPS worker publishes success before a throwing reentrant diagnostic sink")
{
    SoapySidekiq device;
    pps_start_status = 0;
    pps_start_hook = {};
    device.tx_underruns = 99;
    bool logged = false, published = false;
    soapy_sidekiq::ScopeExit restore([]() noexcept { log_hook = {}; });
    log_hook = [&]
    {
        std::scoped_lock lock(device.tx_mutex, device.tx_status_mutex);
        logged = true;
        published =
            device.tx_start_finished && device.tx_start_status == 0 && device.tx_underruns == 0;
        throw std::runtime_error("diagnostic sink failed");
    };
    std::thread worker([&] { device.tx_streaming_start(7); });
    worker.join();
    REQUIRE(logged);
    REQUIRE(published);
    REQUIRE(device.tx_start_finished);
    REQUIRE_EQ(device.tx_start_status, 0);
}

TEST_CASE("unexpected PPS backend exceptions wake writers with a retained startup error")
{
    SoapySidekiq device;
    device.first_transmit = true;
    device.tx_start_finished = false;
    soapy_sidekiq::ScopeExit restore(
        []() noexcept
        {
            pps_start_hook = {};
            wait_hook = {};
        });
    pps_start_hook = [] { throw std::runtime_error("backend failed"); };
    wait_hook = [&] { device.tx_streaming_start(7); };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
    int flags = 0;
    REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 1000000),
               SOAPY_SDR_STREAM_ERROR);
    REQUIRE(device.tx_start_finished);
    REQUIRE_EQ(device.tx_start_status, SOAPY_SDR_STREAM_ERROR);
    REQUIRE(device.first_transmit);
    REQUIRE_EQ(device.attempts, std::size_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    REQUIRE_EQ(device.writeStream(stream, buffers, device.current_tx_block_size, flags, 0, 0),
               SOAPY_SDR_STREAM_ERROR);
}
