/**
 * @file ConstructorChannelCountTests.cpp
 * @brief Checks constructor channel counts before SDK handle-table traversal.
 */

#include "TestHarness.hpp"
#include <array>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#define SoapySDR_logf(...) ((void)0)
namespace
{
struct FakeSdk
{
    std::uint8_t rx_count{2}, tx_count{3};
    int rx_status{}, tx_status{};
    std::vector<std::string> reads;
} sdk;
} // namespace
int skiq_read_num_rx_chans(std::uint8_t, std::uint8_t *count)
{
    sdk.reads.emplace_back("RX");
    *count = sdk.rx_count;
    return sdk.rx_status;
}
int skiq_read_num_tx_chans(std::uint8_t, std::uint8_t *count)
{
    sdk.reads.emplace_back("TX");
    *count = sdk.tx_count;
    return sdk.tx_status;
}

/** Actual constructor statements with matching local/member names and bounded tables. */
std::array<std::uint8_t, 2> queryCounts()
{
    const std::uint8_t card = 5;
    std::uint8_t channels = 0, num_rx_channels = 0, num_tx_channels = 0;
    int status = 0;
    struct Parameters
    {
        unsigned rx_param[2]{};
        unsigned tx_param[3]{};
    } param;
#include "ConstructorChannelCounts.inc"
    return {num_rx_channels, num_tx_channels};
}

TEST_CASE("constructor accepts absent directions and channel counts up to each table capacity")
{
    for (const auto rx : std::array<std::uint8_t, 3>{0, 1, 2})
        for (const auto tx : std::array<std::uint8_t, 3>{0, 1, 3})
        {
            sdk = {};
            sdk.rx_count = rx;
            sdk.tx_count = tx;
            const auto counts = queryCounts();
            REQUIRE_EQ(counts[0], rx);
            REQUIRE_EQ(counts[1], tx);
            REQUIRE(sdk.reads == std::vector<std::string>({"RX", "TX"}));
        }
}

TEST_CASE("constructor rejects oversized SDK counts before traversing that direction table")
{
    for (const bool rx_failure : {false, true})
        for (const auto count :
             std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(rx_failure ? 3 : 4), 255})
        {
            sdk = {};
            (rx_failure ? sdk.rx_count : sdk.tx_count) = count;
            try
            {
                (void)queryCounts();
                REQUIRE(false);
            }
            catch (const std::runtime_error &error)
            {
                const std::string message = error.what();
                REQUIRE(message.find(rx_failure ? "skiq_read_num_rx_chans"
                                                : "skiq_read_num_tx_chans") != std::string::npos);
                REQUIRE(message.find("card 5") != std::string::npos);
                REQUIRE(message.find("channels " + std::to_string(count)) != std::string::npos);
                REQUIRE(message.find(rx_failure ? "table capacity 2" : "table capacity 3") !=
                        std::string::npos);
            }
            REQUIRE_EQ(sdk.reads.size(), rx_failure ? std::size_t{1} : std::size_t{2});
        }
}

TEST_CASE("failed constructor count reads reject partial outputs with SDK context")
{
    for (const bool rx_failure : {false, true})
    {
        sdk = {};
        (rx_failure ? sdk.rx_status : sdk.tx_status) = -5;
        (rx_failure ? sdk.rx_count : sdk.tx_count) = 255;
        try
        {
            (void)queryCounts();
            REQUIRE(false);
        }
        catch (const std::runtime_error &error)
        {
            const std::string message = error.what();
            REQUIRE(message.find(rx_failure ? "skiq_read_num_rx_chans"
                                            : "skiq_read_num_tx_chans") != std::string::npos);
            REQUIRE(message.find("status -5") != std::string::npos);
            REQUIRE(message.find("too many channels") == std::string::npos);
        }
    }
}
