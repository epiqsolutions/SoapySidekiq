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

/** Outcomes returned when a consumer attempts to read RX samples. */
enum class RxReadStatus
{
    samples,
    timeout,
    stopped,
    error
};

/** Outcomes returned when the receive worker submits an RX block. */
enum class RxPushStatus
{
    accepted,
    dropped_oldest,
    stopped
};

/** Metadata and sample count produced by one queue read. */
struct RxReadResult
{
    RxReadStatus status{RxReadStatus::timeout};
    std::size_t samples{};
    std::int64_t time_ns{};
    bool has_time{};
};

/**
 * Thread-safe bounded handoff between the Sidekiq receive worker and SoapySDR.
 *
 * The queue owns a copy of each interleaved CS16 block because Sidekiq retains
 * ownership of its receive buffers. Partial reads retain their exact sample
 * offset so the next read reports the timestamp of its first returned sample.
 */
class RxSampleQueue
{
public:
    /** Construct an inactive queue that can retain at most capacity_blocks. */
    explicit RxSampleQueue(std::size_t capacity_blocks);

    /** Begin a fresh session, discarding data and lifecycle state from the last one. */
    void start();

    /** End the session and wake readers currently waiting for data. */
    void stop();

    /** Mark the producer as failed and wake readers with an error result. */
    void fail();

    /** Return the queue to its initial inactive, empty state. */
    void reset();

    /**
     * Copy one timestamped block into the queue.
     *
     * If the queue is full, the oldest block is discarded so the receive
     * worker never blocks the hardware while waiting for a slow consumer.
     */
    RxPushStatus push(
        const std::int16_t *samples,
        std::size_t complex_samples,
        std::int64_t time_ns,
        std::uint64_t sample_rate);

    /**
     * Read up to requested_samples interleaved complex samples.
     *
     * A successful result may span blocks. Its timestamp always describes the
     * first returned sample, including any offset left by an earlier read.
     */
    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    /** Return whether the queue is accepting producer blocks. */
    bool running() const;

    /** Return the number of blocks currently available to readers. */
    std::size_t queuedBlocks() const;

    /** Return the number of oldest blocks discarded in the current session. */
    std::size_t droppedBlocks() const;

private:
    /** An owned CS16 block plus the next unread complex-sample offset. */
    struct Block
    {
        std::vector<std::int16_t> samples;
        std::size_t offset{};
        std::int64_t time_ns{};
        std::uint64_t sample_rate{};
    };

    /** Calculate the timestamp of the next unread sample in a block. */
    std::int64_t blockTime(const Block &block) const;

    const std::size_t capacity_blocks_;
    mutable std::mutex mutex_;
    std::condition_variable data_available_;
    std::deque<Block> blocks_;
    // All lifecycle and queue fields below are protected by mutex_.
    bool running_{};
    bool failed_{};
    std::size_t dropped_blocks_{};
};

} // namespace soapy_sidekiq
