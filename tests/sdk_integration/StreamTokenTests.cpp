/**
 * @file StreamTokenTests.cpp
 * @brief Exercises concrete stream token, direction, flag, and buffer rejection.
 */
#include "FakeSdk.hpp"
#include "TestHarness.hpp"
#include "SoapySidekiq.hpp"
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
SoapySDR::Kwargs arguments()
{
    return {{"card", "5"}, {"tx_buffer_count", "2"}, {"tx_block_size", "252"}};
}

void requireUnknownToken(SoapySidekiq &device, SoapySDR::Stream *token)
{
    const auto calls_before = scripted_sdk::snapshot().calls;
    int flags = 0;
    long long time = 0;
    std::size_t mask = 0;
    REQUIRE_THROWS_AS(device.getStreamMTU(token), std::invalid_argument);
    REQUIRE_THROWS_AS(device.closeStream(token), std::invalid_argument);
    REQUIRE_EQ(device.activateStream(token), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.deactivateStream(token), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStream(token, nullptr, 1, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.writeStream(token, nullptr, 1, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStreamStatus(token, mask, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE(scripted_sdk::snapshot().calls == calls_before);
}
} // namespace

TEST_CASE("concrete stream methods reject null foreign and closed tokens before SDK calls")
{
    scripted_sdk::reset();
    SoapySidekiq device(arguments());
    auto *rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0});
    auto *tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    requireUnknownToken(device, nullptr);
    requireUnknownToken(device, reinterpret_cast<SoapySDR::Stream *>(std::uintptr_t{1}));
    REQUIRE_EQ(device.getStreamMTU(rx), std::size_t{2});
    REQUIRE_EQ(device.getStreamMTU(tx), std::size_t{252});
    device.closeStream(rx);
    device.closeStream(tx);
    // No replacement allocation: the old values cannot alias a newly owned token.
    requireUnknownToken(device, rx);
    requireUnknownToken(device, tx);
}

TEST_CASE("concrete stream directions and unsupported control flags reject without SDK calls")
{
    scripted_sdk::reset();
    SoapySidekiq device(arguments());
    soapy_sidekiq::ScopeExit cleanup(
        []() noexcept
        {
            scripted_sdk::releaseTxPps();
            scripted_sdk::completeAll();
        });
    auto *rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0});
    auto *tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    const auto calls_before = scripted_sdk::snapshot().calls;
    int flags = 0;
    long long time = 0;
    std::size_t mask = 0;
    REQUIRE_EQ(device.readStream(tx, nullptr, 1, flags, time, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.writeStream(rx, nullptr, 1, flags, 0, 0), SOAPY_SDR_NOT_SUPPORTED);
    REQUIRE_EQ(device.readStreamStatus(rx, mask, flags, time, 0), SOAPY_SDR_NOT_SUPPORTED);
    for (auto *stream : {rx, tx})
    {
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_END_BURST), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, SOAPY_SDR_HAS_TIME, 1), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.activateStream(stream, 0, 0, 1), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_END_BURST), SOAPY_SDR_NOT_SUPPORTED);
        REQUIRE_EQ(device.deactivateStream(stream, SOAPY_SDR_HAS_TIME, 1), SOAPY_SDR_NOT_SUPPORTED);
    }
    REQUIRE(scripted_sdk::snapshot().calls == calls_before);
    REQUIRE(!scripted_sdk::snapshot().rx_active && !scripted_sdk::snapshot().tx_active);
    REQUIRE_EQ(device.activateStream(tx), 0);
    REQUIRE_EQ(device.deactivateStream(tx), 0);
    REQUIRE_EQ(device.activateStream(rx), 0);
    REQUIRE_EQ(device.deactivateStream(rx), 0);
    device.closeStream(tx);
    device.closeStream(rx);
}

TEST_CASE("concrete stream count buffer and timeout validation rejects before transfer")
{
    scripted_sdk::reset();
    SoapySidekiq device(arguments());
    soapy_sidekiq::ScopeExit cleanup([]() noexcept { scripted_sdk::completeAll(); });
    auto *rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0});
    auto *tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    REQUIRE_EQ(device.activateStream(tx), 0);
    const auto mtu = device.getStreamMTU(tx);
    std::vector<std::int16_t> samples(2 * mtu, 1);
    void *output[]{samples.data()};
    const void *input[]{samples.data()};
    const auto calls_before = scripted_sdk::snapshot().calls;
    int flags = 0;
    long long time = 0;
    for (const auto count : {std::size_t{1}, mtu - 1, std::numeric_limits<std::size_t>::max()})
        REQUIRE_EQ(device.writeStream(tx, input, count, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.writeStream(tx, nullptr, mtu, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.writeStream(tx, input, mtu, flags, 0, -1), SOAPY_SDR_STREAM_ERROR);
    flags = SOAPY_SDR_HAS_TIME;
    REQUIRE_EQ(device.writeStream(tx, input, mtu, flags, 1, 0), SOAPY_SDR_NOT_SUPPORTED);
    flags = 0;
    REQUIRE_EQ(device.writeStream(tx, nullptr, 0, flags, 0, 0), 0);
    REQUIRE_EQ(device.readStream(rx, nullptr, 1, flags, time, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device.readStream(rx, output, 1, flags, time, -1), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(
        device.readStream(rx, output, std::numeric_limits<std::size_t>::max(), flags, time, 0),
        SOAPY_SDR_OVERFLOW);
    REQUIRE(scripted_sdk::snapshot().calls == calls_before);
    REQUIRE(scripted_sdk::snapshot().pending.empty());
    REQUIRE_EQ(device.deactivateStream(tx), 0);
    device.closeStream(tx);
    device.closeStream(rx);
}
