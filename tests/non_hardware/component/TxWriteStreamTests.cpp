/**
 * @file TxWriteStreamTests.cpp
 * @brief Verifies production multi-block TX writes with a synchronous fake SDK.
 */

#include "TestHarness.hpp"

#include <cstddef>
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
#define SKIQ_TX_ASYNC_SEND_QUEUE_FULL 1
#define DEFAULT_NUM_BUFFERS 3
#define SoapySDR_logf(...) ((void)0)

namespace SoapySDR { struct Stream {}; }
struct skiq_tx_block_t { std::int16_t data[504]{}; };
class SoapySidekiq;
struct passedStruct { SoapySidekiq *classAddr; std::uint32_t txIndex; };
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
    passedStruct *passedStructInstance{};
    std::mutex tx_buf_mutex;
    std::mutex tx_mutex;
    std::condition_variable tx_cv;
    bool tx_start_finished{};
    int tx_start_status{};
    pthread_mutex_t space_avail_mutex{};
    pthread_cond_t space_avail_cond{};
    std::vector<std::vector<std::int16_t>> transmitted;
    bool complete_on_worker{};

    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    int writeStream(SoapySDR::Stream *, const void *const *, std::size_t,
                    int &, long long, long);
};

/** Capture each accepted packet and complete its ownership synchronously. */
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *block, passedStruct *context)
{
    auto &device = *context->classAddr;
    device.transmitted.emplace_back(block->data, block->data + 2 * device.current_tx_block_size);
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
