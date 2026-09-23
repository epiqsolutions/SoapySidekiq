#include "TestHarness.hpp"

#include <SoapySidekiq/RxSampleQueue.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using soapy_sidekiq::RxPushStatus;
using soapy_sidekiq::RxReadStatus;
using soapy_sidekiq::RxSampleQueue;

TEST_CASE("RX queue requires a positive block capacity")
{
    REQUIRE_THROWS_AS(RxSampleQueue(0), std::invalid_argument);
}

TEST_CASE("RX queue returns complete blocks and their timestamps")
{
    RxSampleQueue queue(2);
    queue.start();
    const std::array<std::int16_t, 6> input{{1, -1, 2, -2, 3, -3}};
    REQUIRE(queue.push(input.data(), 3, 1000, 1000000) == RxPushStatus::accepted);

    std::array<std::int16_t, 6> output{};
    const auto result = queue.read(output.data(), 3, 0us);
    REQUIRE(result.status == RxReadStatus::samples);
    REQUIRE_EQ(result.samples, std::size_t{3});
    REQUIRE(result.has_time);
    REQUIRE_EQ(result.time_ns, std::int64_t{1000});
    REQUIRE(output == input);
}

TEST_CASE("RX queue preserves timestamps across partial reads")
{
    RxSampleQueue queue(2);
    queue.start();
    const std::array<std::int16_t, 8> input{{10, 11, 20, 21, 30, 31, 40, 41}};
    REQUIRE(queue.push(input.data(), 4, 500, 20000000) == RxPushStatus::accepted);

    std::array<std::int16_t, 4> first{};
    const auto first_result = queue.read(first.data(), 2, 0us);
    REQUIRE_EQ(first_result.samples, std::size_t{2});
    REQUIRE_EQ(first_result.time_ns, std::int64_t{500});
    REQUIRE_EQ(first[0], std::int16_t{10});
    REQUIRE_EQ(first[3], std::int16_t{21});

    std::array<std::int16_t, 4> second{};
    const auto second_result = queue.read(second.data(), 2, 0us);
    REQUIRE_EQ(second_result.samples, std::size_t{2});
    REQUIRE_EQ(second_result.time_ns, std::int64_t{600});
    REQUIRE_EQ(second[0], std::int16_t{30});
    REQUIRE_EQ(second[3], std::int16_t{41});
}

TEST_CASE("RX queue combines available blocks in one read")
{
    RxSampleQueue queue(3);
    queue.start();
    const std::array<std::int16_t, 4> first{{1, 2, 3, 4}};
    const std::array<std::int16_t, 4> second{{5, 6, 7, 8}};
    queue.push(first.data(), 2, 100, 1000000);
    queue.push(second.data(), 2, 2100, 1000000);

    std::array<std::int16_t, 8> output{};
    const auto result = queue.read(output.data(), 4, 0us);
    REQUIRE_EQ(result.samples, std::size_t{4});
    REQUIRE_EQ(result.time_ns, std::int64_t{100});
    REQUIRE_EQ(output[0], std::int16_t{1});
    REQUIRE_EQ(output[7], std::int16_t{8});
}

TEST_CASE("RX queue reports timeout without changing output metadata")
{
    RxSampleQueue queue(1);
    queue.start();
    std::array<std::int16_t, 2> output{};
    const auto result = queue.read(output.data(), 1, 1ms);
    REQUIRE(result.status == RxReadStatus::timeout);
    REQUIRE_EQ(result.samples, std::size_t{0});
    REQUIRE(!result.has_time);
}

TEST_CASE("stopping RX wakes a blocked reader")
{
    RxSampleQueue queue(1);
    queue.start();
    std::array<std::int16_t, 2> output{};
    auto reader = std::async(std::launch::async, [&] {
        return queue.read(output.data(), 1, 5s);
    });

    std::this_thread::sleep_for(10ms);
    queue.stop();
    REQUIRE(reader.wait_for(250ms) == std::future_status::ready);
    REQUIRE(reader.get().status == RxReadStatus::stopped);
}

TEST_CASE("RX worker failure wakes a blocked reader with an error")
{
    RxSampleQueue queue(1);
    queue.start();
    std::array<std::int16_t, 2> output{};
    auto reader = std::async(std::launch::async, [&] {
        return queue.read(output.data(), 1, 5s);
    });

    std::this_thread::sleep_for(10ms);
    queue.fail();
    REQUIRE(reader.wait_for(250ms) == std::future_status::ready);
    REQUIRE(reader.get().status == RxReadStatus::error);
}

TEST_CASE("bounded RX queue drops the oldest block")
{
    RxSampleQueue queue(2);
    queue.start();
    const std::array<std::int16_t, 2> first{{1, -1}};
    const std::array<std::int16_t, 2> second{{2, -2}};
    const std::array<std::int16_t, 2> third{{3, -3}};
    REQUIRE(queue.push(first.data(), 1, 100, 1) == RxPushStatus::accepted);
    REQUIRE(queue.push(second.data(), 1, 200, 1) == RxPushStatus::accepted);
    REQUIRE(queue.push(third.data(), 1, 300, 1) == RxPushStatus::dropped_oldest);
    REQUIRE_EQ(queue.droppedBlocks(), std::size_t{1});

    std::array<std::int16_t, 4> output{};
    const auto result = queue.read(output.data(), 2, 0us);
    REQUIRE_EQ(result.samples, std::size_t{2});
    REQUIRE_EQ(result.time_ns, std::int64_t{200});
    REQUIRE_EQ(output[0], std::int16_t{2});
    REQUIRE_EQ(output[2], std::int16_t{3});
}

TEST_CASE("RX queue start and reset clear lifecycle state")
{
    RxSampleQueue queue(1);
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE(queue.push(input.data(), 1, 0, 1) == RxPushStatus::stopped);

    queue.start();
    queue.push(input.data(), 1, 0, 1);
    queue.fail();
    queue.start();
    REQUIRE(queue.running());
    REQUIRE_EQ(queue.queuedBlocks(), std::size_t{0});
    REQUIRE_EQ(queue.droppedBlocks(), std::size_t{0});

    queue.reset();
    REQUIRE(!queue.running());
    REQUIRE(queue.push(input.data(), 1, 0, 1) == RxPushStatus::stopped);
}

TEST_CASE("RX queue validates buffers rates timeouts and timestamp overflow")
{
    RxSampleQueue queue(1);
    queue.start();
    std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_THROWS_AS(queue.push(nullptr, 1, 0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(queue.push(input.data(), 1, 0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(queue.read(nullptr, 1, 0us), std::invalid_argument);
    REQUIRE_THROWS_AS(queue.read(input.data(), 1, -1us), std::invalid_argument);

    queue.push(input.data(), 1, std::numeric_limits<std::int64_t>::max(), 1);
    std::array<std::int16_t, 2> first{};
    REQUIRE_EQ(queue.read(first.data(), 1, 0us).time_ns,
               std::numeric_limits<std::int64_t>::max());

    const std::array<std::int16_t, 4> two_samples{{1, 2, 3, 4}};
    queue.push(two_samples.data(), 2, std::numeric_limits<std::int64_t>::max(), 1);
    REQUIRE_EQ(queue.read(first.data(), 1, 0us).samples, std::size_t{1});
    REQUIRE_THROWS_AS(queue.read(first.data(), 1, 0us), std::overflow_error);
}
