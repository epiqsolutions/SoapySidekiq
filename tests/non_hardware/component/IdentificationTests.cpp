/**
 * @file IdentificationTests.cpp
 * @brief Exercises production device identification and borrowed SDK serial handling.
 */

#include "TestHarness.hpp"
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

#define SoapySDR_logf(...) ((void)0)
namespace SoapySDR
{
using Kwargs = std::map<std::string, std::string>;
}
namespace
{
struct FakeSdk
{
    char serial[12] = "SERIAL-123";
    int status{};
    bool omit_serial{};
    std::uint8_t card{};
    unsigned reads{};
} sdk;
} // namespace

int skiq_read_serial_string(std::uint8_t card, char **serial)
{
    sdk.card = card;
    ++sdk.reads;
    if (!sdk.omit_serial)
        *serial = sdk.serial;
    return sdk.status;
}

/** Supplies only identity state; the actual methods perform SDK reads. */
class SoapySidekiq
{
  public:
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{1};
    std::string part_str{"Sidekiq test card"};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::string getDriverKey() const;
    std::string getHardwareKey() const;
    SoapySDR::Kwargs getHardwareInfo() const;
};

#include "IdentificationMethods.inc"

TEST_CASE("device identity reports SDK serial and direction-specific channel counts")
{
    const SoapySidekiq device;
    REQUIRE_EQ(device.getDriverKey(), std::string{"Sidekiq"});
    REQUIRE_EQ(device.getHardwareKey(), device.part_str);
    const auto info = device.getHardwareInfo();
    REQUIRE_EQ(info.at("card"), std::string{"5"});
    REQUIRE_EQ(info.at("card_type"), device.part_str);
    REQUIRE_EQ(info.at("serial"), std::string{"SERIAL-123"});
    REQUIRE_EQ(info.at("rx_channels"), std::string{"2"});
    REQUIRE_EQ(info.at("tx_channels"), std::string{"1"});
    REQUIRE_EQ(info.at("origin"), std::string{"https://github.com/epiqsolutions/SoapySidekiq"});
    REQUIRE_EQ(sdk.card, std::uint8_t{5});
    REQUIRE_EQ(sdk.reads, 1u);
}

TEST_CASE("hardware info copies the SDK-owned serial without retaining its buffer")
{
    const SoapySidekiq device;
    const auto first = device.getHardwareInfo();
    sdk.serial[0] = 'X';
    const auto second = device.getHardwareInfo();
    REQUIRE_EQ(first.at("serial"), std::string{"SERIAL-123"});
    REQUIRE_EQ(second.at("serial"), std::string{"XERIAL-123"});
}

TEST_CASE("failed serial reads report SDK operation card and status despite partial output")
{
    const SoapySidekiq device;
    sdk.status = -5;
    try
    {
        (void)device.getHardwareInfo();
        REQUIRE(false);
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        REQUIRE(message.find("skiq_read_serial_string") != std::string::npos);
        REQUIRE(message.find("card 5") != std::string::npos);
        REQUIRE(message.find("status -5") != std::string::npos);
    }
}

TEST_CASE("hardware info rejects omitted or empty SDK serials")
{
    const SoapySidekiq device;
    sdk.omit_serial = true;
    REQUIRE_THROWS_AS(device.getHardwareInfo(), std::runtime_error);
    sdk.omit_serial = false;
    sdk.serial[0] = '\0';
    REQUIRE_THROWS_AS(device.getHardwareInfo(), std::runtime_error);
}
