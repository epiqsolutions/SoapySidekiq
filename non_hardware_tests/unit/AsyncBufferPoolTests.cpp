#include "TestHarness.hpp"

#include <SoapySidekiq/AsyncBufferPool.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using soapy_sidekiq::AsyncBufferPool;

TEST_CASE("an empty async buffer pool is rejected")
{
    REQUIRE_THROWS_AS(AsyncBufferPool(0), std::invalid_argument);
}

TEST_CASE("leases reserve distinct buffers")
{
    AsyncBufferPool pool(2);
    auto first = pool.acquireFor(0ms);
    auto second = pool.acquireFor(0ms);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->index() != second->index());
    REQUIRE_EQ(pool.availableCount(), std::size_t{0});
}

TEST_CASE("an unhanded lease automatically releases its buffer")
{
    AsyncBufferPool pool(1);
    {
        auto lease = pool.acquireFor(0ms);
        REQUIRE(lease.has_value());
        REQUIRE_EQ(pool.availableCount(), std::size_t{0});
    }

    REQUIRE_EQ(pool.availableCount(), std::size_t{1});
    REQUIRE(pool.acquireFor(0ms).has_value());
}

TEST_CASE("a handed-off lease remains reserved until completion")
{
    AsyncBufferPool pool(1);
    auto lease = pool.acquireFor(0ms);
    REQUIRE(lease.has_value());
    const auto index = lease->index();
    lease->handOff();
    lease.reset();

    REQUIRE_EQ(pool.availableCount(), std::size_t{0});
    REQUIRE(!pool.acquireFor(0ms).has_value());

    pool.complete(index);
    REQUIRE_EQ(pool.availableCount(), std::size_t{1});
}

TEST_CASE("waiting acquisition wakes after completion")
{
    AsyncBufferPool pool(1);
    auto first = pool.acquireFor(0ms);
    REQUIRE(first.has_value());
    const auto index = first->index();
    first->handOff();

    auto waiter = std::async(std::launch::async, [&pool] {
        return pool.acquireFor(1s).has_value();
    });

    std::this_thread::sleep_for(10ms);
    pool.complete(index);
    REQUIRE(waiter.get());
}

TEST_CASE("waiting acquisition times out when every buffer is busy")
{
    AsyncBufferPool pool(1);
    auto lease = pool.acquireFor(0ms);
    REQUIRE(lease.has_value());

    const auto started = std::chrono::steady_clock::now();
    const auto unavailable = pool.acquireFor(20ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(!unavailable.has_value());
    REQUIRE(elapsed >= 15ms);
}

TEST_CASE("out-of-order completions release the matching buffers")
{
    AsyncBufferPool pool(3);
    auto first = pool.acquireFor(0ms);
    auto second = pool.acquireFor(0ms);
    auto third = pool.acquireFor(0ms);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    const auto first_index = first->index();
    const auto second_index = second->index();
    const auto third_index = third->index();
    first->handOff();
    second->handOff();
    third->handOff();

    pool.complete(second_index);
    auto reused = pool.acquireFor(0ms);
    REQUIRE(reused.has_value());
    REQUIRE_EQ(reused->index(), second_index);

    pool.complete(third_index);
    pool.complete(first_index);
}

TEST_CASE("invalid and duplicate completions are rejected")
{
    AsyncBufferPool pool(1);
    REQUIRE_THROWS_AS(pool.complete(1), std::out_of_range);

    auto lease = pool.acquireFor(0ms);
    REQUIRE(lease.has_value());
    const auto index = lease->index();
    lease->handOff();
    pool.complete(index);
    REQUIRE_THROWS_AS(pool.complete(index), std::logic_error);
}
