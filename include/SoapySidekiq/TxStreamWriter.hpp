#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace soapy_sidekiq
{

enum class TxSendStatus
{
    accepted,
    queue_full,
    error
};

struct TxSendResult
{
    TxSendStatus status{TxSendStatus::error};
    int error_code{};
};

class TxStreamBackend
{
public:
    using Completion = std::function<void(int)>;
    virtual ~TxStreamBackend() = default;
    virtual TxSendResult transmit(
        std::uint32_t handle,
        const std::vector<std::int16_t> &samples,
        Completion completion) = 0;
    virtual int readUnderruns(std::uint32_t handle, std::uint32_t &count) = 0;
};

enum class TxWriteStatus
{
    samples,
    timeout,
    error,
    stopped
};

struct TxWriteResult
{
    TxWriteStatus status{TxWriteStatus::stopped};
    std::size_t samples{};
    int error_code{};
};

class TxStreamWriter
{
public:
    TxStreamWriter(
        TxStreamBackend &backend,
        std::uint32_t handle,
        std::size_t block_samples,
        std::size_t max_in_flight,
        float full_scale);
    ~TxStreamWriter();

    TxStreamWriter(const TxStreamWriter &) = delete;
    TxStreamWriter &operator=(const TxStreamWriter &) = delete;

    void start();
    bool stop(std::chrono::microseconds timeout);

    TxWriteResult writeCs16(
        const std::int16_t *samples,
        std::size_t complex_samples,
        std::chrono::microseconds timeout);
    TxWriteResult writeCf32(
        const float *samples,
        std::size_t complex_samples,
        std::chrono::microseconds timeout);

    int readUnderruns(std::uint32_t &count);
    std::size_t pendingCount() const;
    std::size_t stagedSamples() const;

private:
    struct CompletionState
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t pending{};
        std::size_t generation{};
        int completion_error{};
    };

    TxWriteResult writeConverted(
        const std::int16_t *samples,
        std::size_t complex_samples,
        std::chrono::steady_clock::time_point deadline);
    TxWriteStatus sendStaged(std::chrono::steady_clock::time_point deadline);
    TxStreamBackend &backend_;
    const std::uint32_t handle_;
    const std::size_t block_samples_;
    const std::size_t max_in_flight_;
    const float full_scale_;
    std::shared_ptr<CompletionState> completion_state_;
    mutable std::mutex write_mutex_;
    std::vector<std::int16_t> staging_;
    std::atomic<bool> running_{};
    bool failed_{};
    int send_error_{};
};

} // namespace soapy_sidekiq
