/**
 * @file DeviceOptionsTests.cpp
 * @brief Verifies constructor option defaults, parsing, and rejection paths.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/DeviceOptions.hpp>
#include <SoapySidekiq/ScopeExit.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

using soapy_sidekiq::parseDeviceOptions;

namespace
{

/** Model SDK ownership and injected source failures without opening hardware. */
struct FakeSdk
{
    bool initialized{};
    unsigned exits{};

    void initialize()
    {
        if (initialized) throw std::runtime_error("SDK already initialized");
        initialized = true;
    }

    void shutdown() noexcept
    {
        initialized = false;
        ++exits;
    }
};

/** Exercise the same post-init guard and ownership transfer as the constructor. */
void configureSources(FakeSdk &sdk, const bool fail_clock, const bool fail_time)
{
    sdk.initialize();
    soapy_sidekiq::ScopeExit rollback([&sdk]() noexcept { sdk.shutdown(); });
    if (fail_clock) throw std::runtime_error("clock write failed");
    if (fail_time) throw std::runtime_error("no PPS pulses");
    rollback.release();
}

} // namespace

// These tests exercise the parser without registering callbacks or opening a card.
TEST_CASE("device options require a card and provide a safe TX default")
{
    const auto options = parseDeviceOptions({{"card", "3"}});

    REQUIRE_EQ(options.card, std::uint8_t{3});
    REQUIRE_EQ(options.tx_block_size, soapy_sidekiq::defaultTxBlockSize);
    REQUIRE_EQ(options.tx_buffer_count, soapy_sidekiq::defaultTxBufferCount);
    REQUIRE(!options.topology.has_value());
    REQUIRE(!options.clock_source.has_value());
    REQUIRE(!options.time_source.has_value());
}

TEST_CASE("device options parse every supported constructor argument")
{
    const auto options = parseDeviceOptions({{"card", "255"},
                                             {"topology", "7"},
                                             {"tx_block_size", "4092"},
                                             {"tx_buffer_count", "1024"},
                                             {"clock_source", "external_clock"},
                                             {"time_source", "1pps_source_external"}});

    REQUIRE_EQ(options.card, std::uint8_t{255});
    REQUIRE_EQ(options.topology.value(), std::uint8_t{7});
    REQUIRE_EQ(options.tx_block_size, std::uint32_t{4092});
    REQUIRE_EQ(options.tx_buffer_count, std::uint32_t{1024});
    REQUIRE_EQ(options.clock_source.value(), std::string{"external_clock"});
    REQUIRE_EQ(options.time_source.value(), std::string{"1pps_source_external"});
}

TEST_CASE("device options reject missing malformed and overflowing card values")
{
    REQUIRE_THROWS_AS(parseDeviceOptions({}), std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", ""}}), std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "-1"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "12junk"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "256"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "99999999999999999999"}}),
                      std::invalid_argument);
}

TEST_CASE("device options reject invalid topology and TX block sizes")
{
    REQUIRE_THROWS_AS(
        parseDeviceOptions({{"card", "0"}, {"topology", "256"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        parseDeviceOptions({{"card", "0"}, {"tx_block_size", "0"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        parseDeviceOptions({{"card", "0"}, {"tx_block_size", "65536"}}),
        std::invalid_argument);
}

TEST_CASE("device options reject empty clock and time source names")
{
    REQUIRE_THROWS_AS(
        parseDeviceOptions({{"card", "0"}, {"clock_source", ""}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        parseDeviceOptions({{"card", "0"}, {"time_source", ""}}),
        std::invalid_argument);
}

TEST_CASE("constructor source names reject unsupported choices before hardware acquisition")
{
    for (const auto *key : {"clock_source", "time_source"})
        for (const auto *value :
             {"unknown", "internal", " external_clock", "1pps_source_external "})
            REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "0"}, {key, value}}),
                              std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "0"}, {"clock_source", "1pps_source_external"}}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "0"}, {"time_source", "external_clock"}}),
                      std::invalid_argument);
}

TEST_CASE("constructor source names preserve supported case-insensitive choices")
{
    for (const auto *source :
         {"internal_clock", "external_clock", "INTERNAL_CLOCK", "External_Clock"})
        REQUIRE_EQ(
            parseDeviceOptions({{"card", "0"}, {"clock_source", source}}).clock_source.value(),
            std::string(source));
    for (const auto *source :
         {"1pps_source_unavailable", "1pps_source_external", "1pps_source_internal",
          "1PPS_SOURCE_EXTERNAL", "1pps_Source_Internal"})
        REQUIRE_EQ(parseDeviceOptions({{"card", "0"}, {"time_source", source}}).time_source.value(),
                   std::string(source));
}

TEST_CASE("TX payload sizes obey SDK packet boundaries")
{
    for (const auto *value : {"252", "4092", "16380", "65532"})
    {
        const auto options = parseDeviceOptions({{"card", "0"}, {"tx_block_size", value}});
        REQUIRE_EQ(std::to_string(options.tx_block_size), std::string{value});
    }
    for (const auto *value : {"1", "251", "253", "4096", "65533", "65534", "65535"})
    {
        REQUIRE_THROWS_AS(
            parseDeviceOptions({{"card", "0"}, {"tx_block_size", value}}),
            std::invalid_argument);
    }
}

TEST_CASE("source configuration failures release SDK state and allow retry")
{
    for (const bool fail_clock : {false, true})
    {
        FakeSdk sdk;
        REQUIRE_THROWS_AS(configureSources(sdk, fail_clock, !fail_clock),
                          std::runtime_error);
        REQUIRE(!sdk.initialized);
        REQUIRE_EQ(sdk.exits, 1u);
        configureSources(sdk, false, false);
        REQUIRE(sdk.initialized);
        REQUIRE_EQ(sdk.exits, 1u);
        sdk.shutdown();
    }
}

TEST_CASE("failed SDK acquisition does not release another device's state")
{
    FakeSdk sdk;
    configureSources(sdk, false, false);
    REQUIRE_THROWS_AS(configureSources(sdk, false, false), std::runtime_error);
    REQUIRE(sdk.initialized);
    REQUIRE_EQ(sdk.exits, 0u);
    sdk.shutdown();
}

TEST_CASE("TX buffer count accepts its bounded range independently of block size")
{
    for (const auto *value : {"1", "2", "1024", "30000"})
    {
        const auto options = parseDeviceOptions({{"card", "0"}, {"tx_buffer_count", value}});
        REQUIRE_EQ(std::to_string(options.tx_buffer_count), std::string{value});
        REQUIRE_EQ(options.tx_block_size, soapy_sidekiq::defaultTxBlockSize);
    }
}

TEST_CASE("invalid TX buffer counts fail before any hardware initialization")
{
    for (const auto *value : {"", "0", "30001", "-1", "+1", " 1", "1 ", "1.0", "1x", "4294967296",
                              "999999999999999999999"})
        REQUIRE_THROWS_AS(parseDeviceOptions({{"card", "0"}, {"tx_buffer_count", value}}),
                          std::invalid_argument);
}
