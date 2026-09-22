#include <SoapySidekiq/SampleConversion.hpp>
#include <SoapySidekiq/TxStreamWriter.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace soapy_sidekiq
{

TxStreamWriter::TxStreamWriter(
    TxStreamBackend &backend,
    const std::uint32_t handle,
    const std::size_t block_samples,
    const std::size_t max_in_flight,
    const float full_scale)
    : backend_(backend),
      handle_(handle),
      block_samples_(block_samples),
      max_in_flight_(max_in_flight),
      full_scale_(full_scale),
      completion_state_(std::make_shared<CompletionState>())
{
    if (block_samples_ == 0 ||
        block_samples_ > std::numeric_limits<std::size_t>::max() / 2 ||
        block_samples_ > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument("TX block sample count must be positive");
    }
    if (max_in_flight_ == 0)
    {
        throw std::invalid_argument("TX in-flight limit must be positive");
    }
    if (full_scale_ <= 0.0F)
    {
        throw std::invalid_argument("TX full scale must be positive");
    }
    staging_.reserve(block_samples_ * 2);
}

TxStreamWriter::~TxStreamWriter()
{
    (void)stop(std::chrono::seconds(1));
}

void TxStreamWriter::start()
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    staging_.clear();
    failed_ = false;
    send_error_ = 0;
    running_.store(true);
    std::lock_guard<std::mutex> completion_lock(completion_state_->mutex);
    completion_state_->completion_error = 0;
}

bool TxStreamWriter::stop(const std::chrono::microseconds timeout)
{
    running_.store(false);
    completion_state_->changed.notify_all();
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        staging_.clear();
    }
    std::unique_lock<std::mutex> lock(completion_state_->mutex);
    return completion_state_->changed.wait_for(lock, timeout, [this] {
        return completion_state_->pending == 0;
    });
}

TxWriteResult TxStreamWriter::writeCs16(
    const std::int16_t *samples,
    const std::size_t complex_samples,
    const std::chrono::microseconds timeout)
{
    if (complex_samples != 0 && samples == nullptr)
    {
        throw std::invalid_argument("TX input cannot be null");
    }
    if (timeout.count() < 0)
    {
        throw std::invalid_argument("TX timeout cannot be negative");
    }
    if (complex_samples > std::numeric_limits<std::size_t>::max() / 2)
    {
        throw std::overflow_error("TX sample count is too large");
    }
    return writeConverted(samples, complex_samples, std::chrono::steady_clock::now() + timeout);
}

TxWriteResult TxStreamWriter::writeCf32(
    const float *samples,
    const std::size_t complex_samples,
    const std::chrono::microseconds timeout)
{
    if (complex_samples != 0 && samples == nullptr)
    {
        throw std::invalid_argument("TX input cannot be null");
    }
    if (complex_samples > std::numeric_limits<std::size_t>::max() / 2)
    {
        throw std::overflow_error("TX sample count is too large");
    }
    std::vector<std::int16_t> converted(complex_samples * 2);
    convertCf32ToCs16(samples, converted.data(), complex_samples, full_scale_);
    return writeCs16(converted.data(), complex_samples, timeout);
}

TxWriteResult TxStreamWriter::writeConverted(
    const std::int16_t *samples,
    const std::size_t complex_samples,
    const std::chrono::steady_clock::time_point deadline)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!running_.load())
    {
        return {TxWriteStatus::stopped, 0, 0};
    }
    if (failed_)
    {
        return {TxWriteStatus::error, 0, send_error_};
    }

    std::size_t consumed = 0;
    while (consumed < complex_samples || staging_.size() == block_samples_ * 2)
    {
        if (staging_.size() == block_samples_ * 2)
        {
            const auto send_status = sendStaged(deadline);
            if (send_status != TxWriteStatus::samples)
            {
                return {send_status, consumed, send_error_};
            }
            continue;
        }

        const std::size_t staged_complex = staging_.size() / 2;
        const std::size_t to_copy = std::min(
            block_samples_ - staged_complex, complex_samples - consumed);
        staging_.insert(
            staging_.end(),
            samples + consumed * 2,
            samples + (consumed + to_copy) * 2);
        consumed += to_copy;
    }
    return {TxWriteStatus::samples, consumed, 0};
}

TxWriteStatus TxStreamWriter::sendStaged(
    const std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        std::size_t generation = 0;
        {
            std::unique_lock<std::mutex> state_lock(completion_state_->mutex);
            if (!completion_state_->changed.wait_until(
                    state_lock,
                    deadline,
                    [this] {
                        return completion_state_->pending < max_in_flight_ ||
                               !running_.load();
                    }))
            {
                return TxWriteStatus::timeout;
            }
            if (!running_.load())
            {
                return TxWriteStatus::stopped;
            }
            if (completion_state_->completion_error != 0)
            {
                failed_ = true;
                send_error_ = completion_state_->completion_error;
                staging_.clear();
                return TxWriteStatus::error;
            }
            ++completion_state_->pending;
            generation = completion_state_->generation;
        }

        const auto state = completion_state_;
        const auto result = backend_.transmit(
            handle_, staging_, [state](const int status) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->pending != 0)
                    {
                        --state->pending;
                    }
                    ++state->generation;
                    if (status != 0)
                    {
                        state->completion_error = status;
                    }
                }
                state->changed.notify_all();
            });
        if (result.status == TxSendStatus::accepted)
        {
            staging_.clear();
            return TxWriteStatus::samples;
        }

        {
            std::lock_guard<std::mutex> state_lock(completion_state_->mutex);
            if (completion_state_->pending != 0)
            {
                --completion_state_->pending;
            }
        }
        completion_state_->changed.notify_all();
        if (result.status == TxSendStatus::error)
        {
            failed_ = true;
            send_error_ = result.error_code;
            staging_.clear();
            return TxWriteStatus::error;
        }

        std::unique_lock<std::mutex> state_lock(completion_state_->mutex);
        if (!completion_state_->changed.wait_until(
                state_lock,
                deadline,
                [this, generation] {
                    return completion_state_->generation != generation ||
                           !running_.load();
                }))
        {
            return TxWriteStatus::timeout;
        }
        if (!running_.load())
        {
            return TxWriteStatus::stopped;
        }
    }
}

int TxStreamWriter::readUnderruns(std::uint32_t &count)
{
    return backend_.readUnderruns(handle_, count);
}

std::size_t TxStreamWriter::pendingCount() const
{
    std::lock_guard<std::mutex> lock(completion_state_->mutex);
    return completion_state_->pending;
}

std::size_t TxStreamWriter::stagedSamples() const
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    return staging_.size() / 2;
}

} // namespace soapy_sidekiq
