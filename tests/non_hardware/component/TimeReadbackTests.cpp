/**
 * @file TimeReadbackTests.cpp
 * @brief Exercises production hardware-time readback using mapped SDK handles.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/TimestampConversion.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#define SoapySDR_logf(...) ((void)0)
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
namespace
{
struct FakeSdk
{
    std::uint64_t frequency{40000000}, timestamp{60000000};
    double actual_rate{20000000};
    unsigned frequency_reads{}, timestamp_reads{}, rate_reads{}, handle{};
    int direction{}, frequency_status{}, timestamp_status{}, rate_status{};
    std::uint8_t card{};
    bool omit_rate{};
} sdk;
int readRate(int direction, std::uint8_t card, unsigned handle, std::uint32_t *rate, double *actual,
             std::uint32_t *bandwidth, std::uint32_t *actual_bw)
{
    ++sdk.rate_reads;
    sdk.direction = direction;
    sdk.handle = handle;
    sdk.card = card;
    *rate = 10000000;
    if (!sdk.omit_rate)
        *actual = sdk.actual_rate;
    *bandwidth = *actual_bw = 0; // Not needed to convert timestamps.
    return sdk.rate_status;
}
int readTimestamp(int direction, std::uint8_t card, unsigned handle, std::uint64_t *timestamp)
{
    ++sdk.timestamp_reads;
    sdk.direction = direction;
    sdk.handle = handle;
    sdk.card = card;
    *timestamp = sdk.timestamp;
    return sdk.timestamp_status;
}
} // namespace
int skiq_read_sys_timestamp_freq(std::uint8_t card, std::uint64_t *frequency)
{
    ++sdk.frequency_reads;
    sdk.card = card;
    *frequency = sdk.frequency;
    return sdk.frequency_status;
}
int skiq_read_curr_sys_timestamp(std::uint8_t card, std::uint64_t *timestamp)
{
    return readTimestamp(-1, card, 0, timestamp);
}
int skiq_read_curr_rx_timestamp(std::uint8_t card, unsigned handle, std::uint64_t *timestamp)
{
    return readTimestamp(SOAPY_SDR_RX, card, handle, timestamp);
}
int skiq_read_curr_tx_timestamp(std::uint8_t card, unsigned handle, std::uint64_t *timestamp)
{
    return readTimestamp(SOAPY_SDR_TX, card, handle, timestamp);
}
int skiq_read_rx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t *rate,
                                           double *actual, std::uint32_t *bandwidth,
                                           std::uint32_t *actual_bw)
{
    return readRate(SOAPY_SDR_RX, card, handle, rate, actual, bandwidth, actual_bw);
}
int skiq_read_tx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t *rate,
                                           double *actual, std::uint32_t *bandwidth,
                                           std::uint32_t *actual_bw)
{
    return readRate(SOAPY_SDR_TX, card, handle, rate, actual, bandwidth, actual_bw);
}
/** Timestamp methods and channel validation/mapping come from production. */
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
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{2};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    skiq_rx_hdl_t getFirstRxHandle() const
    {
        return getRxHandle(0);
    }
    skiq_tx_hdl_t getFirstTxHandle() const
    {
        return getTxHandle(0);
    }
    bool hasHardwareTime(const std::string &) const;
    long long getHardwareTime(const std::string &) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "TimeReadbackMethods.inc"

TEST_CASE("default hardware time reads the system timestamp in nanoseconds")
{
    SoapySidekiq device;
    for (const auto &selector : {"", "sys_timestamp", "SYS_TIMESTAMP"})
        REQUIRE_EQ(device.getHardwareTime(selector), 1500000000LL);
    REQUIRE_EQ(sdk.card, device.card);
    REQUIRE_EQ(sdk.rate_reads, 0u);
}
TEST_CASE("hardware time capability reflects available RF directions")
{
    SoapySidekiq device;
    REQUIRE(device.hasHardwareTime(""));
    REQUIRE(device.hasHardwareTime("SYS_TIMESTAMP"));
    REQUIRE(device.hasHardwareTime("RX_RF_TIMESTAMP"));
    REQUIRE(device.hasHardwareTime("TX_RF_TIMESTAMP"));
    REQUIRE(!device.hasHardwareTime("bad"));
    device.num_rx_channels = device.num_tx_channels = 0;
    REQUIRE(!device.hasHardwareTime("rx_rf_timestamp"));
    REQUIRE(!device.hasHardwareTime("tx_rf_timestamp"));
    REQUIRE(device.hasHardwareTime(""));
    REQUIRE_THROWS_AS(device.getHardwareTime("rx_rf_timestamp"), std::out_of_range);
    REQUIRE_THROWS_AS(device.getHardwareTime("tx_rf_timestamp"), std::out_of_range);
    REQUIRE_EQ(sdk.rate_reads + sdk.timestamp_reads, 0u);
}
TEST_CASE("RF timestamps use the first mapped handle and fresh actual rate")
{
    SoapySidekiq device;
    for (const auto &selector : {"rx_rf_timestamp", "TX_RF_TIMESTAMP"})
    {
        REQUIRE_EQ(device.getHardwareTime(selector), 3000000000LL);
        const bool rx = std::string(selector) == "rx_rf_timestamp";
        REQUIRE_EQ(sdk.handle, rx ? 3u : 6u);
        REQUIRE_EQ(sdk.direction, rx ? SOAPY_SDR_RX : SOAPY_SDR_TX);
        REQUIRE_EQ(sdk.card, device.card);
        sdk.actual_rate = 40000000;
        REQUIRE_EQ(device.getHardwareTime(selector), 1500000000LL);
        sdk.actual_rate = 20000000;
    }
}
TEST_CASE("RF readback preserves fractional actual sample rates")
{
    SoapySidekiq device;
    sdk.actual_rate = 10000000.5;
    sdk.timestamp = 100000005;
    REQUIRE_EQ(device.getHardwareTime("rx_rf_timestamp"), 10000000000LL);
    REQUIRE_EQ(device.getHardwareTime("tx_rf_timestamp"), 10000000000LL);
}
TEST_CASE("unknown hardware timestamp selectors fail without SDK access")
{
    SoapySidekiq device;
    for (const auto &selector : {"bad", "now", "1pps", "sys_timestamp "})
        REQUIRE_THROWS_AS(device.getHardwareTime(selector), std::invalid_argument);
    REQUIRE_EQ(sdk.rate_reads + sdk.timestamp_reads + sdk.frequency_reads, 0u);
}
TEST_CASE("invalid or omitted actual RF rates do not return raw ticks")
{
    SoapySidekiq device;
    for (const double rate :
         {0.0, -1.0, 0.5, std::ldexp(1.0, 32), std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
    {
        sdk.actual_rate = rate;
        REQUIRE_THROWS_AS(device.getHardwareTime("rx_rf_timestamp"), std::runtime_error);
        REQUIRE_THROWS_AS(device.getHardwareTime("tx_rf_timestamp"), std::runtime_error);
    }
    sdk.omit_rate = true;
    REQUIRE_THROWS_AS(device.getHardwareTime("rx_rf_timestamp"), std::runtime_error);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
}
TEST_CASE("time readback reports SDK frequency rate and timestamp failures")
{
    SoapySidekiq device;
    sdk.frequency_status = -5;
    REQUIRE_THROWS_AS(device.getHardwareTime(""), std::runtime_error);
    sdk.frequency_status = 0;
    sdk.frequency = 0;
    REQUIRE_THROWS_AS(device.getHardwareTime("sys_timestamp"), std::runtime_error);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
    sdk.frequency = 40000000;
    sdk.rate_status = -6;
    for (const auto &selector : {"rx_rf_timestamp", "tx_rf_timestamp"})
        REQUIRE_THROWS_AS(device.getHardwareTime(selector), std::runtime_error);
    sdk.rate_status = 0;
    sdk.timestamp_status = -7;
    for (const auto &selector : {"", "rx_rf_timestamp", "tx_rf_timestamp"})
        REQUIRE_THROWS_AS(device.getHardwareTime(selector), std::runtime_error);
}
TEST_CASE("hardware timestamp readback rejects overflow and retains large integer precision")
{
    SoapySidekiq device;
    sdk.frequency = 1000000000;
    sdk.actual_rate = 1000000000;
    sdk.timestamp = 9007199254740993ULL;
    for (const auto &selector : {"", "rx_rf_timestamp", "tx_rf_timestamp"})
        REQUIRE_EQ(device.getHardwareTime(selector), 9007199254740993LL);
    sdk.timestamp = std::numeric_limits<std::uint64_t>::max();
    for (const auto &selector : {"", "rx_rf_timestamp", "tx_rf_timestamp"})
        REQUIRE_THROWS_AS(device.getHardwareTime(selector), std::overflow_error);
}
