#include "TestHarness.hpp"

#include <SoapySidekiq/DeviceOptions.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

using soapy_sidekiq::parseDeviceOptions;

// These tests exercise the parser without registering callbacks or opening a card.
TEST_CASE("device options require a card and provide a safe TX default")
{
    const auto options = parseDeviceOptions({{"card", "3"}});

    REQUIRE_EQ(options.card, std::uint8_t{3});
    REQUIRE_EQ(options.tx_block_size, soapy_sidekiq::defaultTxBlockSize);
    REQUIRE(!options.topology.has_value());
    REQUIRE(!options.clock_source.has_value());
    REQUIRE(!options.time_source.has_value());
}

TEST_CASE("device options parse every supported constructor argument")
{
    const auto options = parseDeviceOptions({
        {"card", "255"},
        {"topology", "7"},
        {"tx_block_size", "4096"},
        {"clock_source", "external_clock"},
        {"time_source", "1pps_source_external"}});

    REQUIRE_EQ(options.card, std::uint8_t{255});
    REQUIRE_EQ(options.topology.value(), std::uint8_t{7});
    REQUIRE_EQ(options.tx_block_size, std::uint32_t{4096});
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
