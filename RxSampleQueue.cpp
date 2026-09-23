#include <SoapySidekiq/RxSampleQueue.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace soapy_sidekiq
{

RxSampleQueue::RxSampleQueue(const std::size_t capacity_blocks)
    : capacity_blocks_(capacity_blocks)
{
    if (capacity_blocks_ == 0)
    {
        throw std::invalid_argument("RX queue capacity must be positive");
    }
}

void RxSampleQueue::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A reactivated stream must never expose samples or errors from its prior run.
    blocks_.clear();
    running_ = true;
    failed_ = false;
    dropped_blocks_ = 0;
}

void RxSampleQueue::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    // A reader may otherwise remain asleep until its entire timeout expires.
    data_available_.notify_all();
}

void RxSampleQueue::fail()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        failed_ = true;
    }
    // Failure is terminal for this session and must be visible to every reader.
    data_available_.notify_all();
}

void RxSampleQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        running_ = false;
        failed_ = false;
        dropped_blocks_ = 0;
    }
    data_available_.notify_all();
}

RxPushStatus RxSampleQueue::push(
    const std::int16_t *samples,
    const std::size_t complex_samples,
    const std::int64_t time_ns,
    const std::uint64_t sample_rate)
{
    if (complex_samples == 0 ||
        complex_samples > std::numeric_limits<std::size_t>::max() / 2)
    {
        throw std::invalid_argument("RX sample count must be positive");
    }
    if (samples == nullptr)
    {
        throw std::invalid_argument("RX samples cannot be null");
    }
    if (sample_rate == 0)
    {
        throw std::invalid_argument("RX sample rate cannot be zero");
    }

    // Copying overload keeps raw SDK memory outside the queue's lifetime.
    return push(
        std::vector<std::int16_t>(samples, samples + complex_samples * 2),
        time_ns,
        sample_rate);
}

RxPushStatus RxSampleQueue::push(
    std::vector<std::int16_t> samples,
    const std::int64_t time_ns,
    const std::uint64_t sample_rate)
{
    if (samples.empty() || samples.size() % 2 != 0)
    {
        throw std::invalid_argument("RX sample data must contain complete IQ pairs");
    }
    if (sample_rate == 0)
    {
        throw std::invalid_argument("RX sample rate cannot be zero");
    }

    Block block;
    block.samples = std::move(samples);
    block.time_ns = time_ns;
    block.sample_rate = sample_rate;

    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || failed_)
        {
            return RxPushStatus::stopped;
        }
        if (blocks_.size() == capacity_blocks_)
        {
            // Keep latency bounded without blocking the hardware receive worker.
            blocks_.pop_front();
            ++dropped_blocks_;
            dropped = true;
        }
        blocks_.push_back(std::move(block));
    }
    data_available_.notify_one();
    return dropped ? RxPushStatus::dropped_oldest : RxPushStatus::accepted;
}

std::int64_t RxSampleQueue::blockTime(const Block &block) const
{
    // offset counts complex samples, which advance RF time by one tick each.
    const auto offset_ns = ticksToNanoseconds(block.offset, block.sample_rate);
    if (offset_ns > 0 &&
        block.time_ns > std::numeric_limits<std::int64_t>::max() - offset_ns)
    {
        throw std::overflow_error("RX timestamp does not fit in nanoseconds");
    }
    return block.time_ns + offset_ns;
}

RxReadResult RxSampleQueue::read(
    std::int16_t *output,
    const std::size_t requested_samples,
    const std::chrono::microseconds timeout)
{
    if (requested_samples != 0 && output == nullptr)
    {
        throw std::invalid_argument("RX output cannot be null");
    }
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("RX timeout cannot be negative");
    }
    if (requested_samples == 0)
    {
        return {RxReadStatus::samples, 0, 0, false};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    // Lifecycle transitions share the wake-up path with newly queued samples.
    const auto ready = [this] {
        return !blocks_.empty() || failed_ || !running_;
    };
    if (!ready() && !data_available_.wait_for(lock, timeout, ready))
    {
        return {RxReadStatus::timeout, 0, 0, false};
    }
    if (blocks_.empty())
    {
        return {failed_ ? RxReadStatus::error : RxReadStatus::stopped, 0, 0, false};
    }

    // Capture metadata before consuming blocks; it belongs to the first sample.
    RxReadResult result{RxReadStatus::samples, 0, blockTime(blocks_.front()), true};
    while (result.samples < requested_samples && !blocks_.empty())
    {
        Block &block = blocks_.front();
        const std::size_t block_samples = block.samples.size() / 2;
        const std::size_t available = block_samples - block.offset;
        const std::size_t to_copy =
            std::min(available, requested_samples - result.samples);
        std::memcpy(
            output + result.samples * 2,
            block.samples.data() + block.offset * 2,
            to_copy * 2 * sizeof(std::int16_t));
        block.offset += to_copy;
        result.samples += to_copy;
        if (block.offset == block_samples)
        {
            blocks_.pop_front();
        }
    }
    return result;
}

bool RxSampleQueue::running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::size_t RxSampleQueue::queuedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

std::size_t RxSampleQueue::droppedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_blocks_;
}

} // namespace soapy_sidekiq
