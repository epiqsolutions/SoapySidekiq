/**
 * @file TxWriteStreamTests.cpp
 * @brief Verifies production multi-block TX writes with a synchronous fake SDK.
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

// Accepted sends complete synchronously; PPS startup can succeed or fail.
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
struct skiq_tx_block_t { std::int16_t data[504]{}; };
class SoapySidekiq;
struct passedStruct
{
    inline static std::atomic<std::size_t> live{};
    inline static bool fail_allocation{};
    SoapySidekiq *classAddr{};
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
int skiq_stop_tx_streaming(std::uint8_t, skiq_tx_hdl_t)
{
    ++stop_calls;
    return stop_status;
}
int skiq_stop_tx_streaming_on_1pps(std::uint8_t card, skiq_tx_hdl_t handle, int)
{
    return skiq_stop_tx_streaming(card, handle);
}
int pps_start_status{};
int skiq_start_tx_streaming_on_1pps(std::uint8_t, skiq_tx_hdl_t, int)
{
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

/** Minimal device fixture supplying the state accessed by the production method. */
class SoapySidekiq
{
public:
  struct StreamHandle
  {
      int direction{SOAPY_SDR_TX};
      unsigned tx_handle{}, rx_handle{};
  };
    StreamHandle handle;
    StreamHandle *active_tx_stream{&handle};
    StreamHandle *active_rx_stream{};
    UnusedRxSession *rx_session{};
    bool tx_stream_active{true};
    bool tx_writes_stopping{};
    std::size_t active_tx_writers{}, active_tx_status_readers{};
    std::thread _tx_streaming_thread;
    bool first_transmit{false}, txUseShort{false};
    std::uint32_t current_tx_block_size{252}, currTXBuffIndex{};
    std::uint8_t card{};
    double maxValue{32767};
    double txMaxValue{32767};
    std::uint8_t num_rx_channels{1}, num_tx_channels{1};
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_tx_block_t blocks[DEFAULT_NUM_BUFFERS];
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{&blocks[0], &blocks[1], &blocks[2]};
    std::unique_ptr<std::int32_t[]> p_tx_status{
        std::make_unique<std::int32_t[]>(DEFAULT_NUM_BUFFERS)};
    std::mutex tx_buf_mutex;
    std::mutex tx_mutex;
    FixtureCondition tx_cv;
    FixtureCondition tx_buf_cv;
    std::uint64_t complete_count{};
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

    ~SoapySidekiq()
    {
        for (auto *context : pending)
            delete context;
    }

    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    StreamHandle *findStreamHandle(SoapySDR::Stream *) const noexcept;
    std::timed_mutex tx_status_mutex;
    std::uint32_t tx_underruns{};
    int readStreamStatus(SoapySDR::Stream *, std::size_t &, int &, long long &, long);
    void tx_streaming_start(skiq_tx_hdl_t);
    int writeStream(SoapySDR::Stream *, const void *const *, std::size_t,
                    int &, long long, long);
    void tx_complete(int32_t, skiq_tx_block_t *, uint32_t);
    static void tx_complete_callback(int32_t, skiq_tx_block_t *, void *) noexcept;
    void stopTxWrites();
    int deactivateStream(SoapySDR::Stream *, int, long long);
};

#include "TxCompletionMethod.inc"
#include "TxCompletionCallback.inc"

/** Capture each accepted packet and complete its ownership synchronously. */
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *block, passedStruct *context)
{
    auto &device = *context->classAddr;
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
            const auto prior_index = (context->txIndex + 1) % DEFAULT_NUM_BUFFERS;
            {
                std::lock_guard<std::mutex> lock(device.tx_buf_mutex);
                device.p_tx_status[prior_index] = 1;
            }
            device.tx_complete(0, device.p_tx_block[prior_index], prior_index);
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
                std::unique_lock<std::mutex> lock(device.tx_buf_mutex, std::try_to_lock);
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
    REQUIRE_EQ(device.p_tx_status[0], 0);
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
    device.p_tx_status[0] = 1;
    std::fill(std::begin(device.blocks[0].data), std::end(device.blocks[0].data), 123);
    std::vector<std::int16_t> input(2 * device.current_tx_block_size, 456);
    bool original_payload_preserved = false;
    wait_hook = [&]
    {
        original_payload_preserved =
            std::all_of(std::begin(device.blocks[0].data), std::end(device.blocks[0].data),
                        [](auto sample) { return sample == 123; });
        device.tx_complete(0, device.p_tx_block[0], 0);
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
    REQUIRE_EQ(device.p_tx_status[0], 0);
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
}

TEST_CASE("hard TX errors release reservation and context before a retry")
{
    SoapySidekiq device;
    device.send_results = {-5, 0};
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_THROWS_AS(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                         buffers, device.current_tx_block_size, flags, 0, 0),
                      std::runtime_error);
    REQUIRE_EQ(device.p_tx_status[0], 0);
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
    REQUIRE_THROWS_AS(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                         buffers, device.current_tx_block_size, flags, 0, 0),
                      std::invalid_argument);
    REQUIRE_EQ(device.p_tx_status[0], 0);
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
    REQUIRE_THROWS_AS(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                         buffers, device.current_tx_block_size, flags, 0, 0),
                      std::bad_alloc);
    REQUIRE_EQ(device.p_tx_status[0], 0);
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
    REQUIRE_EQ(device.p_tx_status[0], 1);
    REQUIRE_EQ(device.p_tx_status[1], 1);
    for (auto it = device.pending.rbegin(); it != device.pending.rend(); ++it)
    {
        auto *context = *it;
        device.tx_complete(0, device.p_tx_block[context->txIndex], context->txIndex);
        delete context;
    }
    device.pending.clear();
    REQUIRE_EQ(device.p_tx_status[0], 0);
    REQUIRE_EQ(device.p_tx_status[1], 0);
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
        REQUIRE_EQ(device.p_tx_status[0], 0);
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
    device.p_tx_status[0] = 1;
    std::fill(std::begin(device.blocks[0].data), std::end(device.blocks[0].data), 123);
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    for (long timeout : {0L, 1000L})
    {
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                      device.current_tx_block_size, flags, 0, timeout),
                   SOAPY_SDR_TIMEOUT);
        REQUIRE_EQ(device.attempts, std::size_t{0});
        REQUIRE_EQ(device.p_tx_status[0], 1);
        REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
        REQUIRE(std::all_of(std::begin(device.blocks[0].data), std::end(device.blocks[0].data),
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
        REQUIRE_EQ(device.p_tx_status[0], 0);
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
    REQUIRE_EQ(device.p_tx_status[1], 0);
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
    device.p_tx_status[0] = 1;
    bool notified = false;
    wait_hook = [&] { notified = true; };
    std::vector<float> input(2 * device.current_tx_block_size, 0.25f);
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 1000),
               SOAPY_SDR_TIMEOUT);
    REQUIRE(notified);
    REQUIRE_EQ(device.p_tx_status[0], 1);
    REQUIRE_EQ(device.attempts, std::size_t{0});
}

TEST_CASE("PPS startup and TX buffer waits share one deadline")
{
    SoapySidekiq device;
    device.first_transmit = true;
    device.p_tx_status[0] = 1;
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
    REQUIRE_EQ(device.tx_buf_cv.deadlines.size(), std::size_t{1});
    REQUIRE(device.tx_cv.deadlines[0] == device.tx_buf_cv.deadlines[0]);
    REQUIRE_EQ(device.attempts, std::size_t{0});
}

TEST_CASE("TX buffer completion wakes the maximum timeout without overflow")
{
    SoapySidekiq device;
    device.p_tx_status[0] = 1;
    wait_hook = [&] { device.tx_complete(0, device.p_tx_block[0], 0); };
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
    context->classAddr = &device;
    context->txIndex = 0;
    // An unexpected prior status exercises the diagnostic path.
    device.p_tx_status[0] = 0;
    bool published = false, unlocked = false;
    log_hook = [&]
    {
        std::unique_lock<std::mutex> lock(device.tx_buf_mutex, std::try_to_lock);
        unlocked = lock.owns_lock();
        published = unlocked && device.p_tx_status[0] == 0 && device.complete_count == 1;
        throw std::runtime_error("injected log failure");
    };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    static_assert(noexcept(SoapySidekiq::tx_complete_callback(0, nullptr, nullptr)));
    SoapySidekiq::tx_complete_callback(0, &device.blocks[0], context);
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
    context->classAddr = &device;
    context->txIndex = DEFAULT_NUM_BUFFERS;
    log_hook = [] { throw std::runtime_error("injected log failure"); };
    soapy_sidekiq::ScopeExit reset([]() noexcept { log_hook = {}; });
    SoapySidekiq::tx_complete_callback(0, nullptr, context);
    REQUIRE_EQ(device.complete_count, std::uint64_t{0});
    REQUIRE_EQ(passedStruct::live.load(), std::size_t{0});
    for (int i = 0; i < DEFAULT_NUM_BUFFERS; ++i)
        REQUIRE_EQ(device.p_tx_status[i], 0);
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
        device.p_tx_status[0] = 1;
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
        REQUIRE(device.active_tx_stream == &device.handle);
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
        std::unique_lock<std::mutex> lock(device.tx_buf_mutex);
        admission_closed = device.tx_buf_cv.wait_until(
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
    REQUIRE(!device.tx_buf_cv.deadlines.empty());
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
        std::scoped_lock lock(device.tx_mutex, device.tx_buf_mutex);
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

TEST_CASE("already stopped TX hardware deactivates successfully for immediate and PPS stops")
{
    for (const int flags : {0, SOAPY_SDR_HAS_TIME})
    {
        SoapySidekiq device;
        auto *stream = reinterpret_cast<SoapySDR::Stream *>(&device.handle);
        stop_status = -ENODEV;
        REQUIRE_EQ(device.deactivateStream(stream, flags, 0), 0);
        REQUIRE(!device.tx_stream_active);
        REQUIRE(device.active_tx_stream == &device.handle);
        REQUIRE(device.tx_writes_stopping);
    }
    stop_status = 0;
}
