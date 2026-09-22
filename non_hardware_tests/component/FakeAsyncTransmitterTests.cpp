#include "FakeAsyncTransmitter.hpp"
#include "TestHarness.hpp"

#include <SoapySidekiq/AsyncBufferPool.hpp>

#include <chrono>
#include <cstddef>
#include <memory>

using namespace std::chrono_literals;
using soapy_sidekiq::AsyncBufferPool;

namespace
{

FakeAsyncTransmitter::Result sendOne(
    AsyncBufferPool &pool,
    FakeAsyncTransmitter &transmitter,
    const std::shared_ptr<std::size_t> &live_contexts)
{
    auto lease = pool.acquireFor(0ms);
    REQUIRE(lease.has_value());
    const auto index = lease->index();

    ++*live_contexts;
    const auto result = transmitter.transmit(index, [&pool, live_contexts](const std::size_t completed) {
        pool.complete(completed);
        --*live_contexts;
    });

    if (result == FakeAsyncTransmitter::Result::accepted)
    {
        lease->handOff();
    }
    else
    {
        --*live_contexts;
    }
    return result;
}

} // namespace

TEST_CASE("accepted asynchronous sends retain buffers and callback context")
{
    AsyncBufferPool pool(2);
    FakeAsyncTransmitter transmitter;
    auto live_contexts = std::make_shared<std::size_t>(0);

    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::accepted);
    REQUIRE_EQ(pool.availableCount(), std::size_t{1});
    REQUIRE_EQ(transmitter.pendingCount(), std::size_t{1});
    REQUIRE_EQ(*live_contexts, std::size_t{1});

    transmitter.complete(transmitter.transmittedIndices().front());
    REQUIRE_EQ(pool.availableCount(), std::size_t{2});
    REQUIRE_EQ(transmitter.pendingCount(), std::size_t{0});
    REQUIRE_EQ(*live_contexts, std::size_t{0});
}

TEST_CASE("queue-full rejection releases buffer and callback context")
{
    AsyncBufferPool pool(1);
    FakeAsyncTransmitter transmitter;
    transmitter.enqueueResult(FakeAsyncTransmitter::Result::queue_full);
    auto live_contexts = std::make_shared<std::size_t>(0);

    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::queue_full);
    REQUIRE_EQ(pool.availableCount(), std::size_t{1});
    REQUIRE_EQ(transmitter.pendingCount(), std::size_t{0});
    REQUIRE_EQ(*live_contexts, std::size_t{0});
}

TEST_CASE("hard transmit failure releases buffer and callback context")
{
    AsyncBufferPool pool(1);
    FakeAsyncTransmitter transmitter;
    transmitter.enqueueResult(FakeAsyncTransmitter::Result::error);
    auto live_contexts = std::make_shared<std::size_t>(0);

    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::error);
    REQUIRE_EQ(pool.availableCount(), std::size_t{1});
    REQUIRE_EQ(transmitter.pendingCount(), std::size_t{0});
    REQUIRE_EQ(*live_contexts, std::size_t{0});
}

TEST_CASE("callbacks may complete out of submission order")
{
    AsyncBufferPool pool(3);
    FakeAsyncTransmitter transmitter;
    auto live_contexts = std::make_shared<std::size_t>(0);

    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::accepted);
    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::accepted);
    REQUIRE(sendOne(pool, transmitter, live_contexts)
            == FakeAsyncTransmitter::Result::accepted);

    const auto indices = transmitter.transmittedIndices();
    transmitter.complete(indices[1]);
    transmitter.complete(indices[2]);
    transmitter.complete(indices[0]);

    REQUIRE_EQ(pool.availableCount(), std::size_t{3});
    REQUIRE_EQ(*live_contexts, std::size_t{0});
}

TEST_CASE("many asynchronous completion cycles leave no ownership behind")
{
    AsyncBufferPool pool(8);
    FakeAsyncTransmitter transmitter;
    auto live_contexts = std::make_shared<std::size_t>(0);

    for (std::size_t cycle = 0; cycle < 1000; ++cycle)
    {
        REQUIRE(sendOne(pool, transmitter, live_contexts)
                == FakeAsyncTransmitter::Result::accepted);
        transmitter.complete(transmitter.transmittedIndices().back());
    }

    REQUIRE_EQ(pool.availableCount(), pool.size());
    REQUIRE_EQ(transmitter.pendingCount(), std::size_t{0});
    REQUIRE_EQ(*live_contexts, std::size_t{0});
}
