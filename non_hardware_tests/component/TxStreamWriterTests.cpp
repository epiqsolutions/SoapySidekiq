#include "TestHarness.hpp"

#include <SoapySidekiq/TxStreamWriter.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using soapy_sidekiq::TxSendResult;
using soapy_sidekiq::TxSendStatus;
using soapy_sidekiq::TxStreamBackend;
using soapy_sidekiq::TxStreamWriter;
using soapy_sidekiq::TxWriteStatus;

namespace
{

class FakeTxStreamBackend final : public TxStreamBackend
{
public:
    TxSendResult transmit(
        const std::uint32_t handle,
        const std::vector<std::int16_t> &samples,
        Completion completion) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_.push_back(handle);
        attempts_.push_back(samples);
        const TxSendResult result = scripted_.empty()
            ? TxSendResult{TxSendStatus::accepted, 0}
            : popResult();
        if (result.status == TxSendStatus::accepted)
        {
            accepted_.push_back(samples);
            if (complete_synchronously_)
            {
                completion(synchronous_status_);
            }
            else
            {
                pending_.push_back(std::move(completion));
            }
        }
        return result;
    }

    int readUnderruns(const std::uint32_t handle, std::uint32_t &count) override
    {
        underrun_handle = handle;
        count = underruns;
        return underrun_status;
    }

    void script(const TxSendStatus status, const int error_code = 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scripted_.push_back({status, error_code});
    }

    void complete(const std::size_t index, const int status = 0)
    {
        Completion completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completion = std::move(pending_.at(index));
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        completion(status);
    }

    void setSynchronous(const int status = 0)
    {
        complete_synchronously_ = true;
        synchronous_status_ = status;
    }

    std::size_t pendingCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

    std::vector<std::vector<std::int16_t>> accepted() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepted_;
    }

    std::vector<std::uint32_t> handles() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return handles_;
    }

    std::size_t attemptCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_.size();
    }

    std::uint32_t underruns{};
    int underrun_status{};
    std::uint32_t underrun_handle{};

private:
    TxSendResult popResult()
    {
        const auto result = scripted_.front();
        scripted_.pop_front();
        return result;
    }

    mutable std::mutex mutex_;
    std::deque<TxSendResult> scripted_;
    std::vector<std::vector<std::int16_t>> attempts_;
    std::vector<std::vector<std::int16_t>> accepted_;
    std::vector<std::uint32_t> handles_;
    std::vector<Completion> pending_;
    bool complete_synchronously_{};
    int synchronous_status_{};
};

std::array<std::int16_t, 8> fourSamples()
{
    return {{1, -1, 2, -2, 3, -3, 4, -4}};
}

} // namespace

TEST_CASE("TX writer validates construction")
{
    FakeTxStreamBackend backend;
    REQUIRE_THROWS_AS(TxStreamWriter(backend, 0, 0, 1, 2047.0F),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(TxStreamWriter(backend, 0, 1, 0, 2047.0F),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(TxStreamWriter(backend, 0, 1, 1, 0.0F),
                      std::invalid_argument);
}

TEST_CASE("TX writer validates write arguments")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 0, 1, 1, 2047.0F);
    writer.start();
    REQUIRE_THROWS_AS(writer.writeCs16(nullptr, 1, 0us), std::invalid_argument);
    REQUIRE_THROWS_AS(writer.writeCf32(nullptr, 1, 0us), std::invalid_argument);
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_THROWS_AS(writer.writeCs16(input.data(), 1, -1us), std::invalid_argument);
}

TEST_CASE("TX writer stages partial writes until one hardware block is full")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 9, 4, 2, 2047.0F);
    writer.start();
    const auto input = fourSamples();
    REQUIRE_EQ(writer.writeCs16(input.data(), 2, 0us).samples, std::size_t{2});
    REQUIRE_EQ(writer.stagedSamples(), std::size_t{2});
    REQUIRE_EQ(backend.pendingCount(), std::size_t{0});

    REQUIRE_EQ(writer.writeCs16(input.data() + 4, 2, 0us).samples, std::size_t{2});
    REQUIRE_EQ(writer.stagedSamples(), std::size_t{0});
    REQUIRE_EQ(backend.pendingCount(), std::size_t{1});
    REQUIRE(backend.accepted().front() == std::vector<std::int16_t>(input.begin(), input.end()));
    REQUIRE_EQ(backend.handles().front(), std::uint32_t{9});
    backend.complete(0);
    REQUIRE(writer.stop(50ms));
}

TEST_CASE("TX writer splits multi-block writes and retains the remainder")
{
    FakeTxStreamBackend backend;
    backend.setSynchronous();
    TxStreamWriter writer(backend, 1, 2, 2, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 10> input{{1,2,3,4,5,6,7,8,9,10}};
    const auto result = writer.writeCs16(input.data(), 5, 10ms);
    REQUIRE(result.status == TxWriteStatus::samples);
    REQUIRE_EQ(result.samples, std::size_t{5});
    REQUIRE_EQ(backend.accepted().size(), std::size_t{2});
    REQUIRE_EQ(writer.stagedSamples(), std::size_t{1});
    REQUIRE(writer.stop(50ms));
}

TEST_CASE("TX writer converts and clips CF32 samples")
{
    FakeTxStreamBackend backend;
    backend.setSynchronous();
    TxStreamWriter writer(backend, 1, 2, 1, 100.0F);
    writer.start();
    const std::array<float, 4> input{{-2.0F, -0.5F, 0.5F, 2.0F}};
    REQUIRE_EQ(writer.writeCf32(input.data(), 2, 10ms).samples, std::size_t{2});
    const auto sent = backend.accepted().front();
    REQUIRE_EQ(sent[0], std::int16_t{-100});
    REQUIRE_EQ(sent[1], std::int16_t{-50});
    REQUIRE_EQ(sent[2], std::int16_t{50});
    REQUIRE_EQ(sent[3], std::int16_t{100});
}

TEST_CASE("TX writer times out when every in-flight slot is busy")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_EQ(writer.writeCs16(input.data(), 1, 10ms).samples, std::size_t{1});
    const auto result = writer.writeCs16(input.data(), 1, 1ms);
    REQUIRE(result.status == TxWriteStatus::timeout);
    REQUIRE_EQ(writer.pendingCount(), std::size_t{1});
    backend.complete(0);
    REQUIRE(writer.stop(50ms));
}

TEST_CASE("TX queue-full retries after an earlier completion")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 1, 1, 2, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_EQ(writer.writeCs16(input.data(), 1, 10ms).samples, std::size_t{1});
    backend.script(TxSendStatus::queue_full);

    auto second = std::async(std::launch::async, [&] {
        return writer.writeCs16(input.data(), 1, 250ms);
    });
    while (backend.attemptCount() < 2)
    {
        std::this_thread::yield();
    }
    backend.complete(0);
    REQUIRE(second.wait_for(250ms) == std::future_status::ready);
    REQUIRE(second.get().status == TxWriteStatus::samples);
    REQUIRE_EQ(backend.pendingCount(), std::size_t{1});
    backend.complete(0);
    REQUIRE(writer.stop(50ms));
}

TEST_CASE("TX hard send errors release ownership and fail later writes")
{
    FakeTxStreamBackend backend;
    backend.script(TxSendStatus::error, -7);
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    const auto result = writer.writeCs16(input.data(), 1, 10ms);
    REQUIRE(result.status == TxWriteStatus::error);
    REQUIRE_EQ(result.error_code, -7);
    REQUIRE_EQ(writer.pendingCount(), std::size_t{0});
    REQUIRE(writer.writeCs16(input.data(), 1, 10ms).status == TxWriteStatus::error);
}

TEST_CASE("TX synchronous callbacks release in-flight ownership")
{
    FakeTxStreamBackend backend;
    backend.setSynchronous();
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_EQ(writer.writeCs16(input.data(), 1, 10ms).samples, std::size_t{1});
    REQUIRE_EQ(writer.pendingCount(), std::size_t{0});
}

TEST_CASE("TX callback errors are reported by the next write")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE_EQ(writer.writeCs16(input.data(), 1, 10ms).samples, std::size_t{1});
    backend.complete(0, -11);
    const auto result = writer.writeCs16(input.data(), 1, 10ms);
    REQUIRE(result.status == TxWriteStatus::error);
    REQUIRE_EQ(result.error_code, -11);
}

TEST_CASE("TX stop wakes a writer blocked by back-pressure")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    writer.start();
    const std::array<std::int16_t, 2> input{{1, 2}};
    writer.writeCs16(input.data(), 1, 10ms);
    auto blocked = std::async(std::launch::async, [&] {
        return writer.writeCs16(input.data(), 1, 5s);
    });
    REQUIRE(!writer.stop(1ms));
    REQUIRE(blocked.wait_for(250ms) == std::future_status::ready);
    REQUIRE(blocked.get().status == TxWriteStatus::stopped);
    backend.complete(0);
    REQUIRE(writer.stop(50ms));
}

TEST_CASE("TX writer routes underrun queries")
{
    FakeTxStreamBackend backend;
    backend.underruns = 4;
    TxStreamWriter writer(backend, 12, 1, 1, 2047.0F);
    std::uint32_t count = 0;
    REQUIRE_EQ(writer.readUnderruns(count), 0);
    REQUIRE_EQ(count, std::uint32_t{4});
    REQUIRE_EQ(backend.underrun_handle, std::uint32_t{12});
}

TEST_CASE("TX writes before start or after stop are rejected")
{
    FakeTxStreamBackend backend;
    TxStreamWriter writer(backend, 1, 1, 1, 2047.0F);
    const std::array<std::int16_t, 2> input{{1, 2}};
    REQUIRE(writer.writeCs16(input.data(), 1, 0us).status == TxWriteStatus::stopped);
    writer.start();
    REQUIRE(writer.stop(0us));
    REQUIRE(writer.writeCs16(input.data(), 1, 0us).status == TxWriteStatus::stopped);
}
