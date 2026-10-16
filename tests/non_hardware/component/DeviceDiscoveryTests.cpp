/**
 * @file DeviceDiscoveryTests.cpp
 * @brief Verifies discovery filtering and recoverable backend failures.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/DeviceDiscovery.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct FakeCard
{
    /** Serial returned by the fake backend. */
    std::string serial;
    /** Availability returned by the fake backend. */
    bool available{};
};

/** Scriptable discovery backend with deterministic card ordering and errors. */
class FakeDiscoveryBackend final : public soapy_sidekiq::DiscoveryBackend
{
public:
    /** Populate representative available and unavailable cards. */
    FakeDiscoveryBackend()
    {
        cards.emplace(0, FakeCard{"SERIAL-A", true});
        cards.emplace(2, FakeCard{"SERIAL-B", false});
        cards.emplace(7, FakeCard{"SERIAL-C", true});
    }

    /** @copydoc soapy_sidekiq::DiscoveryBackend::cardIds */
    std::vector<std::uint8_t> cardIds() override
    {
        if (enumeration_failure)
        {
            throw std::runtime_error("scripted enumeration failure");
        }
        std::vector<std::uint8_t> ids;
        for (const auto &entry : cards)
        {
            ids.push_back(entry.first);
        }
        return ids;
    }

    /** @copydoc soapy_sidekiq::DiscoveryBackend::serial */
    std::string serial(const std::uint8_t card) override
    {
        if (serial_failures.count(card) != 0)
        {
            throw std::runtime_error("scripted serial failure");
        }
        return cards.at(card).serial;
    }

    /** @copydoc soapy_sidekiq::DiscoveryBackend::isAvailable */
    bool isAvailable(const std::uint8_t card) override
    {
        if (availability_failures.count(card) != 0)
        {
            throw std::runtime_error("scripted availability failure");
        }
        return cards.at(card).available;
    }

    std::map<std::uint8_t, FakeCard> cards;
    bool enumeration_failure{};
    std::set<std::uint8_t> serial_failures;
    std::set<std::uint8_t> availability_failures;
};

} // namespace

TEST_CASE("discovery returns fresh descriptors for every card")
{
    FakeDiscoveryBackend backend;
    const auto first = soapy_sidekiq::discoverDevices(backend);
    const auto second = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(first.devices.size(), std::size_t{3});
    REQUIRE_EQ(second.devices.size(), std::size_t{3});
    REQUIRE_EQ(first.errors.size(), std::size_t{0});
    REQUIRE_EQ(first.devices[0].serial, std::string{"SERIAL-A"});
    REQUIRE(first.devices[0].available);
    REQUIRE(!first.devices[1].available);
}

TEST_CASE("discovery filters by card number")
{
    FakeDiscoveryBackend backend;
    soapy_sidekiq::DiscoveryQuery query;
    query.card = "7";
    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{7});
    REQUIRE_EQ(result.devices[0].serial, std::string{"SERIAL-C"});
}

TEST_CASE("discovery filters by serial number")
{
    FakeDiscoveryBackend backend;
    soapy_sidekiq::DiscoveryQuery query;
    query.serial = "SERIAL-B";
    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{2});
}

TEST_CASE("card filtering takes precedence when both filters are present")
{
    FakeDiscoveryBackend backend;
    soapy_sidekiq::DiscoveryQuery query;
    query.card = "0";
    query.serial = "SERIAL-C";
    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{0});
}

TEST_CASE("serial failures are reported without using invalid data")
{
    FakeDiscoveryBackend backend;
    backend.serial_failures.insert(2);
    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{2});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE_EQ(result.errors[0].card.value(), std::uint8_t{2});
    REQUIRE_EQ(result.errors[0].operation, std::string{"read serial"});
}

TEST_CASE("availability failures are reported and treated as unavailable")
{
    FakeDiscoveryBackend backend;
    backend.availability_failures.insert(7);
    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{3});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE(!result.devices[2].available);
    REQUIRE_EQ(result.errors[0].operation, std::string{"check availability"});
}

TEST_CASE("enumeration failure produces an empty result with diagnostics")
{
    FakeDiscoveryBackend backend;
    backend.enumeration_failure = true;
    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{0});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE(!result.errors[0].card.has_value());
    REQUIRE_EQ(result.errors[0].operation, std::string{"enumerate cards"});
}
