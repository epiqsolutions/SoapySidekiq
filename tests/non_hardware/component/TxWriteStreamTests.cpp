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
#include <new>
#include <SoapySidekiq/ScopeExit.hpp>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <memory>
#include <pthread.h>
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
#define SOAPY_SDR_NOT_SUPPORTED -1
#define SOAPY_SDR_STREAM_ERROR -2
#define SOAPY_SDR_TIMEOUT -3
#define SKIQ_TX_ASYNC_SEND_QUEUE_FULL 1
#define DEFAULT_NUM_BUFFERS 3
#define SoapySDR_logf(...) ((void)0)

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

/** Minimal device fixture supplying the state accessed by the production method. */
class SoapySidekiq
{
public:
    struct StreamHandle { int direction{SOAPY_SDR_TX}; unsigned tx_handle{}; };
    StreamHandle handle;
    StreamHandle *active_tx_stream{&handle};
    bool first_transmit{false}, txUseShort{false}, space_avail{false};
    std::uint32_t current_tx_block_size{252}, currTXBuffIndex{};
    std::uint8_t card{};
    double maxValue{32767};
    double txMaxValue{32767};
    skiq_tx_block_t blocks[DEFAULT_NUM_BUFFERS];
    skiq_tx_block_t *p_tx_block[DEFAULT_NUM_BUFFERS]{&blocks[0], &blocks[1], &blocks[2]};
    std::unique_ptr<std::int32_t[]> p_tx_status{
        std::make_unique<std::int32_t[]>(DEFAULT_NUM_BUFFERS)};
    std::mutex tx_buf_mutex;
    std::mutex tx_mutex;
    std::condition_variable tx_cv;
    bool tx_start_finished{};
    int tx_start_status{};
    pthread_mutex_t space_avail_mutex{};
    pthread_cond_t space_avail_cond{};
    std::vector<std::vector<std::int16_t>> transmitted;
    bool complete_on_worker{};
    bool defer_completion{};
    std::vector<passedStruct *> pending;
    std::vector<int> send_results;
    std::size_t attempts{};

    SoapySidekiq()
    {
        if (pthread_mutex_init(&space_avail_mutex, nullptr) != 0)
            throw std::runtime_error("fixture mutex initialization");
        if (pthread_cond_init(&space_avail_cond, nullptr) != 0)
        {
            pthread_mutex_destroy(&space_avail_mutex);
            throw std::runtime_error("fixture condition initialization");
        }
    }
    ~SoapySidekiq()
    {
        for (auto *context : pending)
            delete context;
        pthread_cond_destroy(&space_avail_cond);
        pthread_mutex_destroy(&space_avail_mutex);
    }

    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    int writeStream(SoapySDR::Stream *, const void *const *, std::size_t,
                    int &, long long, long);
};

/** Capture each accepted packet and complete its ownership synchronously. */
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *block, passedStruct *context)
{
    auto &device = *context->classAddr;
    const auto attempt = device.attempts++;
    const int status = attempt < device.send_results.size() ? device.send_results[attempt] : 0;
    if (status != 0)
    {
        // Rejected sends never acquire the caller's context. Model an available
        // queue slot so a queue-full test can retry without a real SDK worker.
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL)
            device.space_avail = true;
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
                    device.p_tx_status[context->txIndex] = 0;
                    completed = true;
                }
            });
        completion.join();
        delete context;
        REQUIRE(completed);
    }
    else
    {
        device.p_tx_status[context->txIndex] = 0;
        delete context;
    }
    return 0;
}

static SoapySidekiq::StreamHandle *getStreamHandle(SoapySDR::Stream *stream)
{
    return reinterpret_cast<SoapySidekiq::StreamHandle *>(stream);
}

static soapy_sidekiq::StreamDirection streamDirection(int direction)
{
    if (direction == SOAPY_SDR_RX) return soapy_sidekiq::StreamDirection::rx;
    if (direction == SOAPY_SDR_TX) return soapy_sidekiq::StreamDirection::tx;
    throw std::invalid_argument("invalid stream direction");
}
#include "NativeStreamFormat.inc"

// Inject a completion at the busy-buffer wait boundary without timing sleeps.
std::function<void()> wait_hook;
int fixture_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    if (wait_hook)
    {
        auto hook = std::move(wait_hook);
        wait_hook = {};
        hook();
        return 0;
    }
    return pthread_cond_wait(condition, mutex);
}
#define pthread_cond_wait fixture_cond_wait

// Generated directly from Streaming.cpp by CMake, including the input advance.
#include "TxWriteMethod.inc"

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
        std::lock_guard<std::mutex> lock(device.tx_buf_mutex);
        device.p_tx_status[0] = 0;
    };
    const void *buffers[]{input.data()};
    int flags = 0;
    REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle), buffers,
                                  device.current_tx_block_size, flags, 0, 0),
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
                                  device.current_tx_block_size, flags, 0, 0),
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
        std::lock_guard<std::mutex> lock(device.tx_buf_mutex);
        device.p_tx_status[context->txIndex] = 0;
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
