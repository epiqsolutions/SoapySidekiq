/**
 * @file AntennaTests.cpp
 * @brief Exercises production RF-port metadata without changing RF routing.
 */
#include "TestHarness.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
enum skiq_rf_port_t
{
    skiq_rf_port_unknown,
    port_a,
    port_b
};
namespace
{
struct FakeSdk
{
    std::array<skiq_rf_port_t, 10> ports{};
    int status{}, direction{};
    unsigned calls{}, handle{};
    std::uint8_t card{};
    bool omit_port{}, null_name{}, empty_name{};
} sdk;
int readPort(int direction, std::uint8_t card, unsigned handle, skiq_rf_port_t *port)
{
    ++sdk.calls;
    sdk.direction = direction;
    sdk.card = card;
    sdk.handle = handle;
    if (sdk.status == 0 && !sdk.omit_port)
        *port = sdk.ports.at(handle);
    return sdk.status;
}
} // namespace
int skiq_read_rx_rf_port_for_hdl(std::uint8_t card, unsigned handle, skiq_rf_port_t *port)
{
    return readPort(SOAPY_SDR_RX, card, handle, port);
}
int skiq_read_tx_rf_port_for_hdl(std::uint8_t card, unsigned handle, skiq_rf_port_t *port)
{
    return readPort(SOAPY_SDR_TX, card, handle, port);
}
const char *skiq_rf_port_string(skiq_rf_port_t port)
{
    if (sdk.null_name)
        return nullptr;
    if (sdk.empty_name)
        return "";
    return port == port_a ? "A" : "B";
}
/** SDK tables and port reports are injected; all API methods are production. */
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
        sdk.ports[3] = port_a;
        sdk.ports[7] = port_b;
        sdk.ports[6] = port_b;
        sdk.ports[2] = port_a;
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    std::string currentAntenna(int, std::size_t) const;
    std::vector<std::string> listAntennas(int, std::size_t) const;
    std::string getAntenna(int, std::size_t) const;
    void setAntenna(int, std::size_t, const std::string &);
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "AntennaMethods.inc"

TEST_CASE("antenna metadata uses the selected RX or TX SDK handle")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            const auto expected =
                std::string((direction == SOAPY_SDR_RX) == (channel == 0) ? "A" : "B");
            REQUIRE_EQ(device.getAntenna(direction, channel), expected);
            REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? (channel == 0 ? 3u : 7u)
                                                             : (channel == 0 ? 6u : 2u));
            REQUIRE_EQ(sdk.card, device.card);
            REQUIRE_EQ(sdk.direction, direction);
            const auto antennas = device.listAntennas(direction, channel);
            REQUIRE_EQ(antennas.size(), std::size_t{1});
            REQUIRE_EQ(antennas[0], expected);
        }
}
TEST_CASE("selecting the configured antenna is idempotent and other names are rejected")
{
    SoapySidekiq device;
    device.setAntenna(SOAPY_SDR_RX, 1, "B");
    REQUIRE_EQ(device.getAntenna(SOAPY_SDR_RX, 1), std::string{"B"});
    REQUIRE_THROWS_AS(device.setAntenna(SOAPY_SDR_RX, 1, "A"), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setAntenna(SOAPY_SDR_TX, 1, "B"), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setAntenna(SOAPY_SDR_RX, 0, ""), std::invalid_argument);
    REQUIRE(sdk.ports[7] == port_b);
    REQUIRE(sdk.ports[2] == port_a);
}
TEST_CASE("antenna APIs reject invalid directions and channels before SDK access")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.listAntennas(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getAntenna(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setAntenna(9, 0, "A"), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{2}, std::numeric_limits<std::size_t>::max()})
        {
            REQUIRE_THROWS_AS(device.listAntennas(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.getAntenna(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.setAntenna(direction, channel, "A"), std::out_of_range);
        }
    REQUIRE_EQ(sdk.calls, 0u);
}
TEST_CASE("optional unsupported RF-port reporting returns no fabricated antenna names")
{
    for (const int status : {-ENOTSUP, -ENOSYS})
    {
        SoapySidekiq device;
        sdk.status = status;
        REQUIRE(device.listAntennas(SOAPY_SDR_RX, 0).empty());
        REQUIRE(device.getAntenna(SOAPY_SDR_TX, 0).empty());
        REQUIRE_THROWS_AS(device.setAntenna(SOAPY_SDR_RX, 0, "A"), std::invalid_argument);
    }
}
TEST_CASE("unknown or omitted RF ports return no advertised antenna choice")
{
    SoapySidekiq device;
    sdk.ports[3] = skiq_rf_port_unknown;
    REQUIRE(device.listAntennas(SOAPY_SDR_RX, 0).empty());
    sdk.omit_port = true;
    REQUIRE(device.listAntennas(SOAPY_SDR_TX, 0).empty());
    REQUIRE(device.getAntenna(SOAPY_SDR_RX, 1).empty());
    REQUIRE_THROWS_AS(device.setAntenna(SOAPY_SDR_RX, 0, ""), std::invalid_argument);
}
TEST_CASE("RF-port backend failures are errors rather than UNKNOWN antenna choices")
{
    SoapySidekiq device;
    sdk.status = -EIO;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.listAntennas(direction, 0), std::runtime_error);
        REQUIRE_THROWS_AS(device.getAntenna(direction, 0), std::runtime_error);
        REQUIRE_THROWS_AS(device.setAntenna(direction, 0, "A"), std::runtime_error);
    }
}
TEST_CASE("null or empty RF-port names are rejected before string construction")
{
    SoapySidekiq device;
    sdk.null_name = true;
    REQUIRE_THROWS_AS(device.listAntennas(SOAPY_SDR_RX, 0), std::runtime_error);
    sdk.null_name = false;
    sdk.empty_name = true;
    REQUIRE_THROWS_AS(device.getAntenna(SOAPY_SDR_TX, 0), std::runtime_error);
}
