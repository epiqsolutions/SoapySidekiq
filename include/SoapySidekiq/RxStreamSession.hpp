#pragma once

#include <SoapySidekiq/RxSampleQueue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace soapy_sidekiq
{

enum class RxReceiveStatus
{
    success,
    no_data,
    overrun,
    error
};

struct RxReceiveResult
{
    RxReceiveStatus status{RxReceiveStatus::no_data};
    std::uint32_t handle{};
    std::vector<std::int16_t> samples;
    std::uint64_t rf_timestamp{};
    std::uint64_t system_timestamp{};
    int error_code{};
};

class RxStreamBackend
{
public:
    virtual ~RxStreamBackend() = default;
    virtual int start(std::uint32_t handle, bool on_pps) = 0;
    virtual int stop(std::uint32_t handle, bool on_pps) = 0;
    virtual RxReceiveResult receive() = 0;
};

struct RxSessionConfiguration
{
    std::uint32_t handle{};
    std::size_t block_samples{};
    std::uint64_t sample_rate{};
    std::uint64_t system_timestamp_frequency{};
    bool use_rf_timestamp{true};
    bool on_pps{};
};

class RxStreamSession
{
public:
    explicit RxStreamSession(
        RxStreamBackend &backend,
        std::size_t queue_capacity_blocks = 64);
    ~RxStreamSession();

    RxStreamSession(const RxStreamSession &) = delete;
    RxStreamSession &operator=(const RxStreamSession &) = delete;

    int activate(const RxSessionConfiguration &configuration);
    int deactivate();
    void shutdown() noexcept;

    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    bool running() const;
    bool failed() const;
    std::size_t hardwareOverruns() const;
    std::size_t timestampDiscontinuities() const;
    int receiveError() const;

private:
    void receiveLoop(RxSessionConfiguration configuration) noexcept;

    RxStreamBackend &backend_;
    RxSampleQueue queue_;
    mutable std::mutex lifecycle_mutex_;
    std::thread receive_thread_;
    std::atomic<bool> running_{};
    std::atomic<bool> failed_{};
    std::atomic<std::size_t> hardware_overruns_{};
    std::atomic<std::size_t> timestamp_discontinuities_{};
    std::atomic<int> receive_error_{};
    RxSessionConfiguration active_configuration_{};
    bool active_{};
};

} // namespace soapy_sidekiq
