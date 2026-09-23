/**
 * @file RxSampleQueue.hpp
 * @brief Declares the synchronized, bounded queue used by RX streaming.
 */

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
    /** Logical outcome of the read attempt. */
    RxReadStatus status{RxReadStatus::timeout};
    /** Number of complex samples copied to the caller. */
    std::size_t samples{};
    /** Nanosecond timestamp of the first returned sample. */
    std::int64_t time_ns{};
    /** Whether time_ns contains valid sample metadata. */
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
    /**
     * Construct an inactive queue.
     * @param capacity_blocks Maximum number of retained hardware blocks.
     * @throws std::invalid_argument when capacity_blocks is zero.
     */
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
     *
     * @param samples Source buffer containing interleaved CS16 IQ values.
     * @param complex_samples Number of complex samples in the source buffer.
     * @param time_ns Timestamp of the source block's first sample.
     * @param sample_rate Samples per second used to advance partial timestamps.
     * @return Whether the block was accepted, displaced old data, or was stopped.
     * @throws std::invalid_argument for a nonempty null buffer or zero rate.
     */
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

    /**
     * Read up to requested_samples interleaved complex samples.
     *
     * A successful result may span blocks. Its timestamp always describes the
     * first returned sample, including any offset left by an earlier read.
     *
     * @param output Destination for interleaved CS16 IQ values.
     * @param requested_samples Maximum number of complex samples to copy.
     * @param timeout Maximum time to wait for data or a lifecycle transition.
     * @return Read status, copied sample count, and first-sample timestamp.
     * @throws std::invalid_argument for a nonempty null output or negative timeout.
     * @throws std::overflow_error when an adjusted timestamp exceeds int64_t.
     */
    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    /** @return Whether the queue is accepting producer blocks. */
    bool running() const;

    /** @return Number of whole or partially consumed blocks awaiting reads. */
    std::size_t queuedBlocks() const;

    /** @return Number of oldest blocks discarded in the current session. */
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

    /**
     * Calculate the timestamp of the next unread sample in a block.
     * @param block Queue block whose current offset is being reported.
     * @return Adjusted timestamp in nanoseconds.
     * @throws std::overflow_error when the adjusted timestamp exceeds int64_t.
     */
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
