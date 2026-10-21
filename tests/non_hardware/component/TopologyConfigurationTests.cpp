/**
 * @file TopologyConfigurationTests.cpp
 * @brief Exercises the actual constructor topology selection transaction.
 */

#include "TestHarness.hpp"
#include <SoapySidekiq/DeviceOptions.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>

#define SoapySDR_logf(...) ((void)0)
namespace
{
struct FakeSdk
{
    bool supported{true};
    int apply_status{};
    unsigned queries{}, applies{};
    std::uint8_t card{}, topology{};
} sdk;
} // namespace

bool skiq_is_topology_supported(std::uint8_t card)
{
    ++sdk.queries;
    sdk.card = card;
    return sdk.supported;
}
int skiq_apply_topology(std::uint8_t card, std::uint8_t topology)
{
    ++sdk.applies;
    sdk.card = card;
    sdk.topology = topology;
    return sdk.apply_status;
}

/** Include the production constructor branch with its original local names. */
void configureTopology(const soapy_sidekiq::DeviceOptions &options)
{
    const auto card = options.card;
    int status = 0;
#include "TopologyConfiguration.inc"
}

TEST_CASE("omitted topology leaves the card default unchanged without SDK queries")
{
    sdk = {};
    configureTopology({});
    REQUIRE_EQ(sdk.queries, 0u);
    REQUIRE_EQ(sdk.applies, 0u);
}
TEST_CASE("supported constructor topology requests reach the selected SDK card")
{
    sdk = {};
    soapy_sidekiq::DeviceOptions options;
    options.card = 5;
    options.topology = 17;
    configureTopology(options);
    REQUIRE_EQ(sdk.queries, 1u);
    REQUIRE_EQ(sdk.applies, 1u);
    REQUIRE_EQ(sdk.card, std::uint8_t{5});
    REQUIRE_EQ(sdk.topology, std::uint8_t{17});
}
TEST_CASE("explicit unsupported topology requests raise errors without applying")
{
    sdk = {};
    sdk.supported = false;
    soapy_sidekiq::DeviceOptions options;
    options.card = 5;
    options.topology = 17;
    try
    {
        configureTopology(options);
        REQUIRE(false);
    }
    catch (const std::invalid_argument &error)
    {
        const std::string message = error.what();
        REQUIRE(message.find("17") != std::string::npos);
        REQUIRE(message.find("card 5") != std::string::npos);
    }
    REQUIRE_EQ(sdk.queries, 1u);
    REQUIRE_EQ(sdk.applies, 0u);
}
TEST_CASE("failed SDK topology application preserves operation request and status context")
{
    sdk = {};
    sdk.apply_status = -5;
    soapy_sidekiq::DeviceOptions options;
    options.card = 5;
    options.topology = 17;
    try
    {
        configureTopology(options);
        REQUIRE(false);
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        REQUIRE(message.find("skiq_apply_topology") != std::string::npos);
        REQUIRE(message.find("card 5") != std::string::npos);
        REQUIRE(message.find("topology 17") != std::string::npos);
        REQUIRE(message.find("status -5") != std::string::npos);
    }
}
