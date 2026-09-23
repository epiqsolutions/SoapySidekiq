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

/** Outcomes from a consumer read. */
enum class RxReadStatus
{
    samples,
    timeout,
    stopped,
    error
};

/** Outcomes from adding a hardware block to the bounded queue. */
enum class RxPushStatus
{
    accepted,
    dropped_oldest,
    stopped
};

/** Sample count and timestamp metadata returned by one queue read. */
struct RxReadResult
{
    RxReadStatus status{RxReadStatus::timeout};
    std::size_t samples{};
    std::int64_t time_ns{};
    bool has_time{};
};

/**
 * Thread-safe, bounded queue of interleaved CS16 IQ blocks.
 *
 * The producer may push whole hardware blocks while a consumer reads an
 * arbitrary number of complex samples. Partial-block timestamps are adjusted
 * by the number of samples already consumed.
 */
class RxSampleQueue
{
public:
    /** Create a queue that retains at most capacity_blocks complete blocks. */
    explicit RxSampleQueue(std::size_t capacity_blocks);

    /** Clear prior state and begin accepting blocks. */
    void start();

    /** Stop accepting blocks and wake all waiting consumers. */
    void stop();

    /** Mark the producer failed and wake readers with an error result. */
    void fail();

    /** Clear queued data, counters, and all lifecycle state. */
    void reset();

    /** Copy one interleaved CS16 hardware block into the queue. */
    RxPushStatus push(
        const std::int16_t *samples,
        std::size_t complex_samples,
        std::int64_t time_ns,
        std::uint64_t sample_rate);

    /** Move one owned interleaved CS16 hardware block into the queue. */
    RxPushStatus push(
        std::vector<std::int16_t> samples,
        std::int64_t time_ns,
        std::uint64_t sample_rate);

    /** Read up to requested_samples, waiting no longer than timeout. */
    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    /** Return whether the queue currently accepts producer blocks. */
    bool running() const;

    /** Return the number of whole or partially consumed queued blocks. */
    std::size_t queuedBlocks() const;

    /** Return the number of oldest blocks discarded since start(). */
    std::size_t droppedBlocks() const;

private:
    /** Owned sample block and its current complex-sample read offset. */
    struct Block
    {
        std::vector<std::int16_t> samples;
        std::size_t offset{};
        std::int64_t time_ns{};
        std::uint64_t sample_rate{};
    };

    /** Calculate the timestamp of the block's next unread sample. */
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
