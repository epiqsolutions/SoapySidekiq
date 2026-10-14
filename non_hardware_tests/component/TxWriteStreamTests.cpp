/**
 * @file TxWriteStreamTests.cpp
 * @brief Verifies production multi-block TX writes with a synchronous fake SDK.
 */

#include "TestHarness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <SoapySidekiq/SampleScale.hpp>
#include <SoapySidekiq/SampleConversion.hpp>
#include <SoapySidekiq/StreamConfiguration.hpp>
#include <vector>

// Only the success path is exercised: callbacks immediately release each slot.
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
    std::int32_t p_tx_status[DEFAULT_NUM_BUFFERS]{};
    passedStruct *passedStructInstance{};
    std::mutex tx_buf_mutex;
    pthread_mutex_t tx_enabled_mutex{}, space_avail_mutex{};
    pthread_cond_t tx_enabled_cond{}, space_avail_cond{};
    std::vector<std::vector<std::int16_t>> transmitted;

    std::string getNativeStreamFormat(int, std::size_t, double &) const;
    int writeStream(SoapySDR::Stream *, const void *const *, std::size_t,
                    int &, long long, long);
};

/** Capture each accepted packet and complete its ownership synchronously. */
int skiq_transmit(std::uint8_t, unsigned, skiq_tx_block_t *block, passedStruct *context)
{
    auto &device = *context->classAddr;
    device.transmitted.emplace_back(block->data, block->data + 2 * device.current_tx_block_size);
    device.p_tx_status[context->txIndex] = 0;
    delete context;
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
            REQUIRE_EQ(device.transmitted[block][word], soapy_sidekiq::convertNormalizedFloatToSample(
                input[block * words_per_block + word], static_cast<float>(device.txMaxValue)));
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

TEST_CASE("TX CF32 uses DAC scale when RX and TX widths differ")
{
    for (const std::uint8_t bits : {std::uint8_t{12}, std::uint8_t{14}, std::uint8_t{16}})
    {
        SoapySidekiq device;
        device.txMaxValue = soapy_sidekiq::sampleFullScale(bits);
        const std::size_t words = 2 * device.current_tx_block_size;
        std::vector<float> input(3 * words);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = i % 2 == 0 ? 0.5f : -0.5f;
        const void *buffers[]{input.data()};
        int flags = 0;
        REQUIRE_EQ(device.writeStream(reinterpret_cast<SoapySDR::Stream *>(&device.handle),
                                      buffers, input.size() / 2, flags, 0, 0),
                   static_cast<int>(input.size() / 2));
        for (const auto &block : device.transmitted)
            for (std::size_t i = 0; i < words; ++i)
                REQUIRE_EQ(block[i], soapy_sidekiq::convertNormalizedFloatToSample(input[i], static_cast<float>(device.txMaxValue)));
    }
}

TEST_CASE("native CS16 full scale distinguishes ADC and DAC widths")
{
    SoapySidekiq device;
    device.maxValue = soapy_sidekiq::sampleFullScale(16);
    device.txMaxValue = soapy_sidekiq::sampleFullScale(14);
    double scale = 0;
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_RX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 32767.0);
    REQUIRE_EQ(device.getNativeStreamFormat(SOAPY_SDR_TX, 0, scale), std::string{"CS16"});
    REQUIRE_EQ(scale, 8191.0);
    REQUIRE_THROWS_AS(device.getNativeStreamFormat(99, 0, scale), std::invalid_argument);
}
