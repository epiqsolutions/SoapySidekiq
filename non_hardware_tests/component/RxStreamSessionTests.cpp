#include "TestHarness.hpp"

#include <SoapySidekiq/RxStreamSession.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using soapy_sidekiq::RxReadStatus;
using soapy_sidekiq::RxReceiveResult;
using soapy_sidekiq::RxReceiveStatus;
using soapy_sidekiq::RxSessionConfiguration;
using soapy_sidekiq::RxStreamBackend;
using soapy_sidekiq::RxStreamSession;

namespace
{

/** Blocking backend fake used to exercise real worker wakeup and join paths. */
class FakeRxBackend final : public RxStreamBackend
{
public:
    int start(std::uint32_t handle, bool on_pps) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++start_calls;
        started_handle = handle;
        started_on_pps = on_pps;
        if (start_status == 0)
        {
            streaming_ = true;
        }
        return start_status;
    }

    int stop(std::uint32_t handle, bool on_pps) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stop_calls;
            stopped_handle = handle;
            stopped_on_pps = on_pps;
            streaming_ = false;
        }
        // Release a receive() blocked on an empty scripted event queue.
        event_available_.notify_all();
        return stop_status;
    }

    RxReceiveResult receive() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        event_available_.wait(lock, [this] {
            return !events_.empty() || !streaming_;
        });
        if (events_.empty())
        {
            return {RxReceiveStatus::no_data, 0, {}, 0, 0, 0};
        }
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    /** Make one receive result available to the session worker. */
    void enqueue(RxReceiveResult event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(std::move(event));
        }
        event_available_.notify_one();
    }

    int start_status{};
    int stop_status{};
    int start_calls{};
    int stop_calls{};
    std::uint32_t started_handle{};
    std::uint32_t stopped_handle{};
    bool started_on_pps{};
    bool stopped_on_pps{};

private:
    std::mutex mutex_;
    std::condition_variable event_available_;
    std::deque<RxReceiveResult> events_;
    bool streaming_{};
};

/** Return a valid two-complex-sample session configuration. */
RxSessionConfiguration configuration(
    const bool use_rf_timestamp = true,
    const bool on_pps = false)
{
    return {7, 2, 20000000, 1000000, use_rf_timestamp, on_pps};
}

/** Build a successful receive event with explicit handle and timestamps. */
RxReceiveResult samples(
    const std::uint32_t handle,
    std::vector<std::int16_t> values,
    const std::uint64_t rf_timestamp,
    const std::uint64_t system_timestamp = 0)
{
    return {
        RxReceiveStatus::success,
        handle,
        std::move(values),
        rf_timestamp,
        system_timestamp,
        0};
}

} // namespace

TEST_CASE("RX session starts receives and stops through its backend")
{
    FakeRxBackend backend;
    RxStreamSession session(backend, 4);
    REQUIRE_EQ(session.activate(configuration(true, true)), 0);
    REQUIRE_EQ(backend.start_calls, 1);
    REQUIRE_EQ(backend.started_handle, std::uint32_t{7});
    REQUIRE(backend.started_on_pps);

    backend.enqueue(samples(7, {1, -1, 2, -2}, 20));
    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE(result.status == RxReadStatus::samples);
    REQUIRE_EQ(result.samples, std::size_t{2});
    REQUIRE_EQ(result.time_ns, std::int64_t{1000});
    REQUIRE_EQ(output[0], std::int16_t{1});
    REQUIRE_EQ(output[3], std::int16_t{-2});

    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(backend.stop_calls, 1);
    REQUIRE_EQ(backend.stopped_handle, std::uint32_t{7});
    REQUIRE(backend.stopped_on_pps);
}

TEST_CASE("RX session supports system timestamp selection")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration(false)), 0);
    backend.enqueue(samples(7, {1, 2, 3, 4}, 99, 3));

    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE_EQ(result.time_ns, std::int64_t{3000});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session ignores blocks for a different handle")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue(samples(8, {9, 9, 9, 9}, 10));
    backend.enqueue(samples(7, {1, 2, 3, 4}, 12));

    std::array<std::int16_t, 4> output{};
    const auto result = session.read(output.data(), 2, 250ms);
    REQUIRE_EQ(result.samples, std::size_t{2});
    REQUIRE_EQ(output[0], std::int16_t{1});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session continues receiving after a no-data result")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::no_data, 0, {}, 0, 0, 0});
    backend.enqueue(samples(7, {1, 2, 3, 4}, 12));

    std::array<std::int16_t, 4> output{};
    REQUIRE_EQ(session.read(output.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session counts overruns and timestamp discontinuities")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::overrun, 0, {}, 0, 0, 0});
    backend.enqueue(samples(7, {1, 2, 3, 4}, 100));
    backend.enqueue(samples(7, {5, 6, 7, 8}, 200));

    std::array<std::int16_t, 4> first{};
    std::array<std::int16_t, 4> second{};
    REQUIRE_EQ(session.read(first.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE_EQ(session.read(second.data(), 2, 250ms).samples, std::size_t{2});
    REQUIRE_EQ(session.hardwareOverruns(), std::size_t{1});
    REQUIRE_EQ(session.timestampDiscontinuities(), std::size_t{1});
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session propagates fatal receive errors to blocked reads")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue({RxReceiveStatus::error, 0, {}, 0, 0, -77});

    std::array<std::int16_t, 2> output{};
    const auto result = session.read(output.data(), 1, 250ms);
    REQUIRE(result.status == RxReadStatus::error);
    REQUIRE(session.failed());
    REQUIRE_EQ(session.receiveError(), -77);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX session rejects malformed hardware block sizes")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    backend.enqueue(samples(7, {1, 2}, 0));

    std::array<std::int16_t, 2> output{};
    const auto result = session.read(output.data(), 1, 250ms);
    REQUIRE(result.status == RxReadStatus::error);
    REQUIRE(session.failed());
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX start failures do not leave an active worker")
{
    FakeRxBackend backend;
    backend.start_status = -5;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), -5);
    REQUIRE(!session.running());
    REQUIRE_EQ(backend.stop_calls, 0);

    backend.start_status = 0;
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), 0);
}

TEST_CASE("RX stop failures are returned after the worker exits")
{
    FakeRxBackend backend;
    backend.stop_status = -9;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), -9);
    REQUIRE(!session.running());
}

TEST_CASE("RX session destruction unblocks a waiting receive")
{
    FakeRxBackend backend;
    {
        RxStreamSession session(backend);
        REQUIRE_EQ(session.activate(configuration()), 0);
        // Destructor must stop the fake so its blocked receive() can return.
    }
    REQUIRE_EQ(backend.stop_calls, 1);
}

TEST_CASE("RX deactivation wakes a blocked stream reader")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    REQUIRE_EQ(session.activate(configuration()), 0);
    std::array<std::int16_t, 2> output{};
    auto reader = std::async(std::launch::async, [&] {
        return session.read(output.data(), 1, 5s);
    });

    // Deactivation must notify the queue instead of waiting for five seconds.
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE(reader.wait_for(250ms) == std::future_status::ready);
    REQUIRE(reader.get().status == RxReadStatus::stopped);
}

TEST_CASE("RX session validates configuration and repeated activation")
{
    FakeRxBackend backend;
    RxStreamSession session(backend);
    auto invalid = configuration();
    invalid.block_samples = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);
    invalid = configuration();
    invalid.sample_rate = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);
    invalid = configuration(false);
    invalid.system_timestamp_frequency = 0;
    REQUIRE_THROWS_AS(session.activate(invalid), std::invalid_argument);

    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_THROWS_AS(session.activate(configuration()), std::logic_error);
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(session.deactivate(), 0);
    REQUIRE_EQ(session.activate(configuration()), 0);
    REQUIRE_EQ(session.deactivate(), 0);
}
