/**
 * @file TimeSettingTests.cpp
 * @brief Verifies production timestamp updates, selectors, and SDK error paths.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/TimestampConversion.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
namespace
{
struct FakeSdk
{
    std::uint64_t frequency{40000000}, rf{}, system{};
    int read_status{}, write_status{};
    unsigned reads{}, immediate_writes{}, pps_writes{};
    std::uint8_t card{};
    bool omit_frequency{};
} sdk;
} // namespace
int skiq_read_sys_timestamp_freq(std::uint8_t card, std::uint64_t *frequency)
{
    ++sdk.reads;
    sdk.card = card;
    if (!sdk.omit_frequency)
        *frequency = sdk.frequency;
    return sdk.read_status;
}
int skiq_update_timestamps(std::uint8_t card, std::uint64_t timestamp)
{
    ++sdk.immediate_writes;
    sdk.card = card;
    if (!sdk.write_status)
        sdk.rf = sdk.system = timestamp;
    return sdk.write_status;
}
int skiq_write_timestamp_update_on_1pps(std::uint8_t card, std::uint64_t rf, std::uint64_t system)
{
    ++sdk.pps_writes;
    sdk.card = card;
    if (!sdk.write_status)
    {
        sdk.rf = rf;
        sdk.system = system;
    }
    return sdk.write_status;
}
/** Only backend configuration is injected; timestamp setter is production. */
class SoapySidekiq
{
  public:
    bool configuration_blocked{};
    unsigned configuration_attempts{};
    // Inject admission here; real retained-ownership serialization is covered
    // by RxLifecycleTests and RxStreamSessionTests.
    void configureWhileRxInactive(const std::function<void()> &configure)
    {
        ++configuration_attempts;
        if (configuration_blocked)
            throw std::logic_error("RX owns hardware");
        configure();
    }
    std::uint8_t card{5};
    SoapySidekiq()
    {
        sdk = {};
    }
    void setHardwareTime(long long, const std::string &);
};
#include "TimeSettingMethods.inc"

TEST_CASE("hardware time defaults to an immediate update and accepts case insensitive selectors")
{
    for (const auto &selector : {"", "now", "NOW"})
    {
        SoapySidekiq device;
        device.setHardwareTime(1000000000, selector);
        REQUIRE_EQ(sdk.immediate_writes, 1u);
        REQUIRE_EQ(sdk.pps_writes, 0u);
        REQUIRE_EQ(sdk.rf, std::uint64_t{40000000});
        REQUIRE_EQ(sdk.system, sdk.rf);
        REQUIRE_EQ(sdk.card, device.card);
    }
}
TEST_CASE("PPS hardware time updates preserve RF zero and system clock units")
{
    SoapySidekiq device;
    device.setHardwareTime(1500000000, "1PPS");
    REQUIRE_EQ(sdk.pps_writes, 1u);
    REQUIRE_EQ(sdk.immediate_writes, 0u);
    REQUIRE_EQ(sdk.rf, std::uint64_t{0});
    REQUIRE_EQ(sdk.system, std::uint64_t{60000000});
}
TEST_CASE("invalid time selectors and negative times are rejected before SDK access")
{
    SoapySidekiq device;
    for (const auto &selector : {"bad", "sys_timestamp", "now ", "pps"})
        REQUIRE_THROWS_AS(device.setHardwareTime(0, selector), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setHardwareTime(-1, "now"), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setHardwareTime(std::numeric_limits<long long>::min(), "1pps"),
                      std::invalid_argument);
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.immediate_writes + sdk.pps_writes, 0u);
}
TEST_CASE("large hardware time updates retain integer precision and truncate fractional ticks")
{
    SoapySidekiq device;
    sdk.frequency = 1000000000;
    device.setHardwareTime(9007199254740993LL, "now");
    REQUIRE_EQ(sdk.system, std::uint64_t{9007199254740993ULL});
    sdk.frequency = 3;
    device.setHardwareTime(1500000000, "now");
    REQUIRE_EQ(sdk.system, std::uint64_t{4});
    device.setHardwareTime(0, "now");
    REQUIRE_EQ(sdk.system, std::uint64_t{0});
}
TEST_CASE("zero missing and overflowing timestamp frequencies prevent writes")
{
    SoapySidekiq device;
    sdk.frequency = 0;
    REQUIRE_THROWS_AS(device.setHardwareTime(0, "now"), std::invalid_argument);
    sdk.frequency = 40000000;
    sdk.omit_frequency = true;
    REQUIRE_THROWS_AS(device.setHardwareTime(0, "1pps"), std::invalid_argument);
    sdk.omit_frequency = false;
    sdk.frequency = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_THROWS_AS(device.setHardwareTime(1000000001, "now"), std::overflow_error);
    REQUIRE_EQ(sdk.immediate_writes + sdk.pps_writes, 0u);
}
TEST_CASE("SDK timestamp update errors report the selected operation")
{
    for (const auto &selector : {"now", "1pps"})
    {
        SoapySidekiq device;
        sdk.write_status = -5;
        std::string message;
        try
        {
            device.setHardwareTime(0, selector);
        }
        catch (const std::runtime_error &error)
        {
            message = error.what();
        }
        const auto operation = std::string(selector) == "now"
                                   ? "skiq_update_timestamps"
                                   : "skiq_write_timestamp_update_on_1pps";
        REQUIRE(message.find(std::string(operation) + " failed: -5") != std::string::npos);
    }
}
TEST_CASE("SDK frequency read failure prevents immediate and PPS updates")
{
    SoapySidekiq device;
    sdk.read_status = -7;
    for (const auto &selector : {"now", "1pps"})
        REQUIRE_THROWS_AS(device.setHardwareTime(0, selector), std::runtime_error);
    REQUIRE_EQ(sdk.immediate_writes + sdk.pps_writes, 0u);
}

TEST_CASE("RX ownership rejects immediate and PPS time updates before clock reads")
{
    SoapySidekiq device;
    device.configuration_blocked = true;
    for (const auto &selector : {"", "now", "1pps"})
        REQUIRE_THROWS_AS(device.setHardwareTime(1500000000, selector), std::logic_error);
    REQUIRE_EQ(sdk.reads + sdk.immediate_writes + sdk.pps_writes, 0u);
    REQUIRE_EQ(sdk.rf + sdk.system, std::uint64_t{0});
    device.configuration_blocked = false;
    device.setHardwareTime(1500000000, "now");
    REQUIRE_EQ(sdk.immediate_writes, 1u);
    REQUIRE_EQ(sdk.system, std::uint64_t{60000000});
}
