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

/** Result categories returned by an RX hardware backend. */
enum class RxReceiveStatus
{
    success,
    no_data,
    overrun,
    error
};

/** One normalized receive result, independent of the Sidekiq SDK types. */
struct RxReceiveResult
{
    RxReceiveStatus status{RxReceiveStatus::no_data};
    std::uint32_t handle{};
    std::vector<std::int16_t> samples;
    std::uint64_t rf_timestamp{};
    std::uint64_t system_timestamp{};
    int error_code{};
};

/** Hardware boundary used by RxStreamSession and its non-hardware tests. */
class RxStreamBackend
{
public:
    virtual ~RxStreamBackend() = default;

    /** Start an RX handle, optionally synchronized to PPS. */
    virtual int start(std::uint32_t handle, bool on_pps) = 0;

    /** Stop an RX handle using the same synchronization mode as start(). */
    virtual int stop(std::uint32_t handle, bool on_pps) = 0;

    /** Wait for and normalize the next hardware receive result. */
    virtual RxReceiveResult receive() = 0;
};

/** Immutable settings captured for one active receive session. */
struct RxSessionConfiguration
{
    std::uint32_t handle{};
    std::size_t block_samples{};
    std::uint64_t sample_rate{};
    std::uint64_t system_timestamp_frequency{};
    bool use_rf_timestamp{true};
    bool on_pps{};
};

/**
 * Owns the RX worker lifecycle and transfers normalized hardware blocks into a
 * thread-safe RxSampleQueue. The session always joins its worker before it is
 * destroyed, including after backend failures.
 */
class RxStreamSession
{
public:
    /** Construct a session around a backend that must outlive this object. */
    explicit RxStreamSession(
        RxStreamBackend &backend,
        std::size_t queue_capacity_blocks = 64);

    /** Stop the backend and join the receive worker if still active. */
    ~RxStreamSession();

    RxStreamSession(const RxStreamSession &) = delete;
    RxStreamSession &operator=(const RxStreamSession &) = delete;

    /** Start hardware and its receive worker; return the backend status. */
    int activate(const RxSessionConfiguration &configuration);

    /** Stop hardware, wake readers, join the worker, and return stop status. */
    int deactivate();

    /** Best-effort, non-throwing teardown for destructors and device cleanup. */
    void shutdown() noexcept;

    /** Read queued complex samples with the caller's exact timeout. */
    RxReadResult read(
        std::int16_t *output,
        std::size_t requested_samples,
        std::chrono::microseconds timeout);

    /** Return true while the receive worker should continue running. */
    bool running() const;

    /** Return true after a fatal receive or validation error. */
    bool failed() const;

    /** Return the number of hardware overrun notifications seen this session. */
    std::size_t hardwareOverruns() const;

    /** Return the number of non-contiguous RF timestamps seen this session. */
    std::size_t timestampDiscontinuities() const;

    /** Return the backend error code saved by the receive worker. */
    int receiveError() const;

private:
    /** Worker entry point; all failures are translated into session state. */
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
