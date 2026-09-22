#include "FakeSidekiqDevice.hpp"
#include "TestHarness.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("fake Sidekiq discovery supports card and serial lookup")
{
    FakeSidekiqDevice device;
    device.addCard({0, "SERIAL-A", true, 2, 1, 12});
    device.addCard({3, "SERIAL-B", false, 1, 1, 14});

    REQUIRE_EQ(device.cards().size(), std::size_t{2});
    REQUIRE(device.findCardById(0) != nullptr);
    REQUIRE_EQ(device.findCardById(0)->rx_channels, std::size_t{2});
    REQUIRE(device.findCardById(2) == nullptr);
    REQUIRE(device.findCardBySerial("SERIAL-B") != nullptr);
    REQUIRE(device.findCardBySerial("missing") == nullptr);
}

TEST_CASE("fake Sidekiq initialization enforces availability and ownership")
{
    FakeSidekiqDevice device;
    device.addCard({0, "AVAILABLE", true});
    device.addCard({1, "BUSY", false});

    REQUIRE(!device.initialize(1));
    REQUIRE(device.initialize(0));
    REQUIRE_EQ(device.initializedCard().value(), std::uint8_t{0});
    REQUIRE(!device.initialize(0));

    device.shutdown();
    REQUIRE(!device.initializedCard().has_value());
    REQUIRE(device.initialize(0));
}

TEST_CASE("fake RX defaults to no data and replays scripted events")
{
    FakeSidekiqDevice device;
    device.addCard({0, "RX", true});
    REQUIRE(device.initialize(0));

    auto event = device.receive();
    REQUIRE(event.first == FakeSidekiqDevice::RxResult::no_data);
    REQUIRE(!event.second.has_value());

    device.enqueueRxResult(FakeSidekiqDevice::RxResult::overrun);
    device.enqueueRxBlock({100, 200, {1, -1, 2, -2}});

    event = device.receive();
    REQUIRE(event.first == FakeSidekiqDevice::RxResult::overrun);
    event = device.receive();
    REQUIRE(event.first == FakeSidekiqDevice::RxResult::success);
    REQUIRE_EQ(event.second->rf_timestamp, std::uint64_t{100});
    REQUIRE_EQ(event.second->samples.size(), std::size_t{4});
}

TEST_CASE("fake RX and TX reject operations before initialization")
{
    FakeSidekiqDevice device;
    REQUIRE_THROWS_AS(device.receive(), std::logic_error);
    REQUIRE_THROWS_AS(
        device.transmit(0, [](std::size_t, int) {}),
        std::logic_error);
    REQUIRE_THROWS_AS(
        device.enqueueRxResult(FakeSidekiqDevice::RxResult::success),
        std::invalid_argument);
}

TEST_CASE("fake TX scripts queue-full, error, and accepted results")
{
    FakeSidekiqDevice device;
    device.addCard({0, "TX", true});
    REQUIRE(device.initialize(0));
    device.enqueueTxResult(FakeSidekiqDevice::TxResult::queue_full);
    device.enqueueTxResult(FakeSidekiqDevice::TxResult::error);
    device.enqueueTxResult(FakeSidekiqDevice::TxResult::accepted);

    std::size_t completions = 0;
    const auto callback = [&completions](std::size_t, int) { ++completions; };
    REQUIRE(device.transmit(4, callback) == FakeSidekiqDevice::TxResult::queue_full);
    REQUIRE(device.transmit(4, callback) == FakeSidekiqDevice::TxResult::error);
    REQUIRE(device.transmit(4, callback) == FakeSidekiqDevice::TxResult::accepted);
    REQUIRE_EQ(device.pendingTxCount(), std::size_t{1});
    REQUIRE_EQ(completions, std::size_t{0});

    device.completeTx(4);
    REQUIRE_EQ(completions, std::size_t{1});
    REQUIRE_EQ(device.pendingTxCount(), std::size_t{0});
}

TEST_CASE("fake TX callbacks retain status and may complete out of order")
{
    FakeSidekiqDevice device;
    device.addCard({0, "TX", true});
    REQUIRE(device.initialize(0));

    std::vector<std::string> completions;
    device.transmit(2, [&completions](const std::size_t index, const int status) {
        completions.push_back(std::to_string(index) + ":" + std::to_string(status));
    });
    device.transmit(7, [&completions](const std::size_t index, const int status) {
        completions.push_back(std::to_string(index) + ":" + std::to_string(status));
    });

    device.completeTx(7, -5);
    device.completeTx(2, 0);

    REQUIRE_EQ(completions.size(), std::size_t{2});
    REQUIRE_EQ(completions[0], std::string{"7:-5"});
    REQUIRE_EQ(completions[1], std::string{"2:0"});
}

TEST_CASE("fake shutdown drops pending callbacks")
{
    FakeSidekiqDevice device;
    device.addCard({0, "TX", true});
    REQUIRE(device.initialize(0));
    device.transmit(1, [](std::size_t, int) {});
    REQUIRE_EQ(device.pendingTxCount(), std::size_t{1});

    device.shutdown();
    REQUIRE_EQ(device.pendingTxCount(), std::size_t{0});
    REQUIRE_THROWS_AS(device.completeTx(1), std::logic_error);
}
