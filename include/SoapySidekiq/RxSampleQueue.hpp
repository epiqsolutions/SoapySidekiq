#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace soapy_sidekiq
{

enum class RxReadStatus
{
    samples,
    timeout,
    stopped,
    error
};

enum class RxPushStatus
{
    accepted,
    dropped_oldest,
    stopped
};

struct RxReadResult
{
    RxReadStatus status{RxReadStatus::timeout};
    std::size_t samples{};
    std::int64_t time_ns{};
    bool has_time{};
};

class RxSampleQueue
{
public:
    explicit RxSampleQueue(std::size_t capacity_blocks);

    void start();
    void stop();
    void fail();
    void reset();

    RxPushStatus push(
        const std::int16_t *samples,
        std::size_t complex_samples,
        std::int64_t time_ns,
        std::uint64_t sample_rate);
    RxPushStatus push(
        std::vector<std::int16_t> samples,
        std::int64_t time_ns,
        std::uint64_t sample_rate);

    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    bool running() const;
    std::size_t queuedBlocks() const;
    std::size_t droppedBlocks() const;

private:
    struct Block
    {
        std::vector<std::int16_t> samples;
        std::size_t offset{};
        std::int64_t time_ns{};
        std::uint64_t sample_rate{};
    };

    std::int64_t blockTime(const Block &block) const;

    const std::size_t capacity_blocks_;
    mutable std::mutex mutex_;
    std::condition_variable data_available_;
    std::deque<Block> blocks_;
    bool running_{};
    bool failed_{};
    std::size_t dropped_blocks_{};
};

} // namespace soapy_sidekiq
