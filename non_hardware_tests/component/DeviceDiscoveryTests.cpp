#include "FakeSidekiqDevice.hpp"
#include "TestHarness.hpp"

#include <SoapySidekiq/DeviceDiscovery.hpp>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

class FakeDiscoveryBackend final : public soapy_sidekiq::DiscoveryBackend
{
public:
    explicit FakeDiscoveryBackend(FakeSidekiqDevice &device)
        : device_(device)
    {
    }

    std::vector<std::uint8_t> cardIds() override
    {
        if (enumeration_failure)
        {
            throw std::runtime_error("scripted enumeration failure");
        }

        std::vector<std::uint8_t> ids;
        for (const auto &card : device_.cards())
        {
            ids.push_back(card.id);
        }
        return ids;
    }

    std::string serial(const std::uint8_t card) override
    {
        if (serial_failures.count(card) != 0)
        {
            throw std::runtime_error("scripted serial failure");
        }
        const auto *found = device_.findCardById(card);
        if (found == nullptr)
        {
            throw std::runtime_error("unknown card");
        }
        return found->serial;
    }

    bool isAvailable(const std::uint8_t card) override
    {
        if (availability_failures.count(card) != 0)
        {
            throw std::runtime_error("scripted availability failure");
        }
        const auto *found = device_.findCardById(card);
        if (found == nullptr)
        {
            throw std::runtime_error("unknown card");
        }
        return found->available;
    }

    bool enumeration_failure{};
    std::set<std::uint8_t> serial_failures;
    std::set<std::uint8_t> availability_failures;

private:
    FakeSidekiqDevice &device_;
};

FakeSidekiqDevice makeDiscoveryDevice()
{
    FakeSidekiqDevice device;
    device.addCard({0, "SERIAL-A", true, 2, 1, 12});
    device.addCard({2, "SERIAL-B", false, 1, 1, 14});
    device.addCard({7, "SERIAL-C", true, 1, 2, 16});
    return device;
}

} // namespace

TEST_CASE("production discovery returns fresh descriptors for every card")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);

    const auto first = soapy_sidekiq::discoverDevices(backend);
    const auto second = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(first.devices.size(), std::size_t{3});
    REQUIRE_EQ(second.devices.size(), std::size_t{3});
    REQUIRE_EQ(first.errors.size(), std::size_t{0});
    REQUIRE_EQ(first.devices[0].serial, std::string{"SERIAL-A"});
    REQUIRE(first.devices[0].available);
    REQUIRE(!first.devices[1].available);
}

TEST_CASE("production discovery filters by card number")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    soapy_sidekiq::DiscoveryQuery query;
    query.card = "7";

    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{7});
    REQUIRE_EQ(result.devices[0].serial, std::string{"SERIAL-C"});
}

TEST_CASE("production discovery filters by serial number")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    soapy_sidekiq::DiscoveryQuery query;
    query.serial = "SERIAL-B";

    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{2});
}

TEST_CASE("card filtering takes precedence when both filters are present")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    soapy_sidekiq::DiscoveryQuery query;
    query.card = "0";
    query.serial = "SERIAL-C";

    const auto result = soapy_sidekiq::discoverDevices(backend, query);

    REQUIRE_EQ(result.devices.size(), std::size_t{1});
    REQUIRE_EQ(result.devices[0].card, std::uint8_t{0});
}

TEST_CASE("serial failures are reported without using invalid data")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    backend.serial_failures.insert(2);

    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{2});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE_EQ(result.errors[0].card.value(), std::uint8_t{2});
    REQUIRE_EQ(result.errors[0].operation, std::string{"read serial"});
}

TEST_CASE("availability failures are reported and treated as unavailable")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    backend.availability_failures.insert(7);

    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{3});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE(!result.devices[2].available);
    REQUIRE_EQ(result.errors[0].operation, std::string{"check availability"});
}

TEST_CASE("enumeration failure produces an empty result with diagnostics")
{
    auto device = makeDiscoveryDevice();
    FakeDiscoveryBackend backend(device);
    backend.enumeration_failure = true;

    const auto result = soapy_sidekiq::discoverDevices(backend);

    REQUIRE_EQ(result.devices.size(), std::size_t{0});
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE(!result.errors[0].card.has_value());
    REQUIRE_EQ(result.errors[0].operation, std::string{"enumerate cards"});
}
