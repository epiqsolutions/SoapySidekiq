/**
 * @file TxStreaming.cpp
 * @brief Implements TX buffer ownership, completion, startup, writes, and status.
 */
#include "SoapySidekiq.hpp"
#include <SoapySidekiq/Diagnostic.hpp>
#include <SoapySidekiq/SampleConversion.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#ifdef SOAPYSIDEKIQ_DRIVER_TEST_HOOKS
#include "tests/sdk_integration/DriverTestHooks.hpp"
#endif

void SoapySidekiq::tx_complete_callback(int32_t status, skiq_tx_block_t *p_data,
                                        void *p_user) noexcept
{
    (void)status;
    (void)p_data;
    // Completion owns the accepted submission's context, including on every
    // diagnostic/exception path. Never unwind through the SDK's C boundary.
    std::unique_ptr<TxSendContext> context(static_cast<TxSendContext *>(p_user));
    try
    {
        if (context == nullptr || context->completion == nullptr)
        {
            SoapySDR_log(SOAPY_SDR_ERROR, "tx_complete callback received invalid user data");
            return;
        }

        completeTx(context->completion, context->txIndex);
    }
    catch (...)
    {
        // A failing log sink must not escape or be retried from this callback.
    }
}

void SoapySidekiq::completeTx(const std::shared_ptr<TxCompletion> &completion, uint32_t txIndex)
{
    if (completion == nullptr || txIndex >= completion->slots.size())
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received invalid buffer index %u", txIndex);
        return;
    }
    bool unexpected_status;
    {
        std::lock_guard<std::mutex> lock(completion->mutex);
        unexpected_status = completion->slots[txIndex] != 1;
        completion->slots[txIndex] = 0;
        ++completion->completed;
    }
    completion->changed.notify_all();
    // Publish before calling a potentially throwing/reentrant diagnostic sink.
    if (unexpected_status)
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received a buffer that was not reserved");
}

void SoapySidekiq::stopTxWrites()
{
    {
        std::scoped_lock lock(tx_mutex, tx_completion->mutex);
        tx_writes_stopping = true;
    }
    tx_cv.notify_all();
    tx_completion->changed.notify_all();
    std::unique_lock<std::mutex> lock(tx_completion->mutex);
    tx_completion->changed.wait(
        lock, [this] { return active_tx_writers == 0 && active_tx_status_readers == 0; });
}

int SoapySidekiq::stopTxStream(bool on_pps)
{
    stopTxWrites();
    // Cancelling a writer does not cancel SDK startup. Join after the SDK call
    // returns (edge or error), before stopping hardware or destroying device state.
    if (_tx_streaming_thread.joinable())
    {
#ifdef SOAPYSIDEKIQ_DRIVER_TEST_HOOKS
        scripted_sdk::driverTestPoint(scripted_sdk::DriverTestPoint::join_pps_worker);
#endif
        _tx_streaming_thread.join();
    }
    if (!tx_stream_active || active_tx_stream == nullptr)
        return 0;
    const auto tx_handle = active_tx_stream->tx_handle;
    const int status = on_pps ? skiq_stop_tx_streaming_on_1pps(card, tx_handle, 0)
                              : skiq_stop_tx_streaming(card, tx_handle);
    if (status != 0 && status != -ENODEV)
        return status;
    tx_stream_active = false;
    return 0;
}

void SoapySidekiq::shutdownTx() noexcept
{
    try
    {
        const int status = stopTxStream(false);
        if (status != 0)
            SoapySDR_logf(SOAPY_SDR_ERROR, "TX shutdown stop failed (card %u), status %d", card,
                          status);
    }
    catch (...)
    {
        // Destruction must continue even if a diagnostic sink throws.
    }
    active_tx_stream.reset();
    // Pending contexts retain accepted blocks and completion storage themselves.
    tx_blocks.reset();
}

void SoapySidekiq::tx_streaming_start(skiq_tx_hdl_t tx_handle) noexcept
{
    int status = SOAPY_SDR_STREAM_ERROR;
    try
    {
        status = skiq_start_tx_streaming_on_1pps(card, tx_handle, 0);
    }
    catch (...)
    {
        // Treat an unexpected backend exception like a failed asynchronous start.
    }
    {
        std::scoped_lock lock(tx_mutex, tx_status_mutex);
        if (status == 0)
            tx_underruns = 0; // SDK resets its counter on start.
        tx_start_status = status;
        tx_start_finished = true;
    }
    tx_cv.notify_all();

    // Publish and wake waiters before invoking a user-installed diagnostic sink.
    try
    {
        if (status != 0)
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_start_tx_streaming_on_1pps failed (card %u, handle %u), status %d",
                          card, tx_handle, status);
        else
            SoapySDR_logf(SOAPY_SDR_INFO, "TX start streaming on 1pps completed");
    }
    catch (...)
    {
        // Retrying a failed sink could recurse or throw again.
    }
}

void SoapySidekiq::initializeTxBuffers()
{
    auto completion = std::make_shared<TxCompletion>(tx_buffer_count);
    auto blocks = std::make_unique<TxBlocks>(tx_buffer_count, current_tx_block_size,
                                             skiq_tx_block_allocate, skiq_tx_block_free);
    tx_completion = std::move(completion);
    tx_blocks = std::move(blocks);
    tx_block_index = 0;
}

int SoapySidekiq::writeStream(SoapySDR::Stream *stream, const void *const *buffs,
                              const size_t numElems, int &flags, const long long timeNs,
                              const long timeoutUs)
{
    (void)timeNs; // Timed writes are rejected by the flags check below.
    int status = 0;
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    if (stream_handle->direction != SOAPY_SDR_TX)
    {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    if (active_tx_stream.get() != stream_handle)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    // TX is continuous/immediate. Timed writes and burst/fragment flags
    // cannot be honored by the current SDK data-flow configuration.
    if (flags != 0)
        return SOAPY_SDR_NOT_SUPPORTED;

    if (timeoutUs < 0)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(tx_completion->mutex);
        if (tx_writes_stopping)
            return SOAPY_SDR_STREAM_ERROR;
        ++active_tx_writers;
    }
    soapy_sidekiq::ScopeExit writer(
        [this]() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(tx_completion->mutex);
                --active_tx_writers;
            }
            tx_completion->changed.notify_all();
        });

    // TX has no partial-block staging. Reject malformed requests before PPS
    // waits, pointer access, reservations, or submission. The API return type
    // must be able to represent every accepted sample count.
    if (current_tx_block_size == 0 ||
        numElems > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        numElems % current_tx_block_size != 0)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }
    if (numElems == 0)
        return 0;
    if (buffs == nullptr || buffs[0] == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    // Clamp the deadline so even LONG_MAX microseconds cannot overflow
    // the clock's (typically nanosecond) representation.
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto budget = std::chrono::microseconds(timeoutUs);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - now);
    const auto deadline = budget >= remaining ? Clock::time_point::max() : now + budget;

    if (first_transmit)
    {
        soapy_sidekiq::reportDiagnostic(
            [&] { SoapySDR_logf(SOAPY_SDR_DEBUG, "writeStream waiting for PPS TX start"); });
#ifdef SOAPYSIDEKIQ_DRIVER_TEST_HOOKS
        scripted_sdk::driverTestPoint(scripted_sdk::DriverTestPoint::writer_pps_wait);
#endif
        std::unique_lock<std::mutex> lock(tx_mutex);
        if (!tx_cv.wait_until(lock, deadline,
                              [this] { return tx_start_finished || tx_writes_stopping; }))
        {
            // The asynchronous PPS start is still pending. A later write may
            // wait again; no packet or callback context has been submitted.
            return SOAPY_SDR_TIMEOUT;
        }
        if (tx_writes_stopping || tx_start_status != 0)
        {
            // Keep the gate set so retries cannot transmit after a failed start.
            return SOAPY_SDR_STREAM_ERROR;
        }
        first_transmit = false;
    }

    // Pointer to the location in the input buffer to transmit from
    const char *inbuff_ptr = static_cast<const char *>(buffs[0]);

    // numElems already fits the API's int return type, and block size is positive.
    const auto num_blocks = static_cast<uint32_t>(numElems / current_tx_block_size);

    uint32_t curr_block = 0;

    // total number of bytes that need to be transmitted in this call
    uint32_t tx_block_bytes = current_tx_block_size * 4;
    // Hardware payloads are CS16, but the caller's CF32 buffer uses wider samples.
    const size_t input_block_bytes =
        current_tx_block_size * 2 * (txUseShort ? sizeof(int16_t) : sizeof(float));

    while (curr_block < num_blocks)
    {
        uint64_t observed_completions = 0;
        {
            // The predicate and completion updates share a mutex, so completion
            // cannot disappear between testing the slot and sleeping.
            std::unique_lock<std::mutex> lock(tx_completion->mutex);
            if (!tx_completion->changed.wait_until(
                    lock, deadline,
                    [this]
                    { return tx_writes_stopping || tx_completion->slots[tx_block_index] == 0; }))
            {
                return curr_block == 0 ? SOAPY_SDR_TIMEOUT
                                       : static_cast<int>(curr_block * current_tx_block_size);
            }
            if (tx_writes_stopping)
                return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                       : static_cast<int>(curr_block * current_tx_block_size);
            tx_completion->slots[tx_block_index] = 1;
            observed_completions = tx_completion->completed;
        }

        try
        {
            // Until the SDK accepts this block, unwind its reservation on any
            // allocation, conversion, or submission failure.
            const auto block_index = tx_block_index;
            soapy_sidekiq::ScopeExit reservation(
                [this, block_index]() noexcept
                {
                    std::lock_guard<std::mutex> lock(tx_completion->mutex);
                    tx_completion->slots[block_index] = 0;
                });
            auto context = std::make_unique<TxSendContext>();
            context->completion = tx_completion;
            context->block = tx_blocks->block(block_index);
            context->txIndex = block_index;

            // Pointer to the location in the output buffer to copy to.
            char *outbuff_ptr = reinterpret_cast<char *>(context->block->data);

            // determine if we received short or float
            if (txUseShort)
            {
                // CS16
                memcpy(outbuff_ptr, inbuff_ptr, tx_block_bytes);
            }
            else
            {
                // Clip and normalize exceptional CF32 values before integer storage.
                soapy_sidekiq::convertCf32ToCs16(reinterpret_cast<const float *>(inbuff_ptr),
                                                 reinterpret_cast<std::int16_t *>(outbuff_ptr),
                                                 current_tx_block_size,
                                                 static_cast<float>(this->txMaxValue));
            }

            status = skiq_transmit(this->card, stream_handle->tx_handle, context->block.get(),
                                   context.get());
            if (status == 0)
            {
                // The SDK completion callback now owns context. It may already
                // have run synchronously; do not inspect context after submit.
                context.release();
                reservation.release();
            }
        }
        catch (const std::exception &error)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(
                        SOAPY_SDR_ERROR,
                        "TX packet preparation/submission failed (card %u, handle %u): %s", card,
                        stream_handle->tx_handle, error.what());
                });
            return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                   : static_cast<int>(curr_block * current_tx_block_size);
        }
        if (status == SKIQ_TX_ASYNC_SEND_QUEUE_FULL)
        {
            // A completion during skiq_transmit must satisfy this predicate
            // immediately; wait only when no completion has made progress.
            std::unique_lock<std::mutex> lock(tx_completion->mutex);
            const bool progressed =
                tx_writes_stopping ||
                (Clock::now() < deadline &&
                 tx_completion->changed.wait_until(lock, deadline,
                                                   [this, observed_completions] {
                                                       return tx_writes_stopping ||
                                                              tx_completion->completed !=
                                                                  observed_completions;
                                                   }));
            if (tx_writes_stopping)
                return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                       : static_cast<int>(curr_block * current_tx_block_size);
            if (!progressed)
            {
                return curr_block == 0 ? SOAPY_SDR_TIMEOUT
                                       : static_cast<int>(curr_block * current_tx_block_size);
            }
        }
        else if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_transmit failed (card %u, handle %u), status %d", card,
                                  stream_handle->tx_handle, status);
                });
            // Accepted packets cannot be rolled back. Report their prefix so
            // the caller retries only the unsent suffix after a hard SDK error.
            return curr_block == 0 ? SOAPY_SDR_STREAM_ERROR
                                   : static_cast<int>(curr_block * current_tx_block_size);
        }
        else
        {
            curr_block++;

            // move the index into the transmit block array
            tx_block_index = (tx_block_index + 1) % tx_buffer_count;

            // move the pointer to the next block in the writeStream buffer
            inbuff_ptr += input_block_bytes;
        }
    }

    return static_cast<int>(numElems);
}

int SoapySidekiq::readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags,
                                   long long &timeNs, const long timeoutUs)
{
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if (stream_handle->direction != SOAPY_SDR_TX)
        return SOAPY_SDR_NOT_SUPPORTED;
    if (active_tx_stream.get() != stream_handle)
        return SOAPY_SDR_STREAM_ERROR;
    chanMask = 0;
    flags = 0;
    timeNs = 0;
    if (timeoutUs < 0)
        return SOAPY_SDR_STREAM_ERROR;

    {
        std::lock_guard<std::mutex> lock(tx_completion->mutex);
        if (tx_writes_stopping)
            return SOAPY_SDR_STREAM_ERROR;
        ++active_tx_status_readers;
    }
    soapy_sidekiq::ScopeExit reader(
        [this]() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(tx_completion->mutex);
                --active_tx_status_readers;
            }
            tx_completion->changed.notify_all();
        });

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto budget = std::chrono::microseconds(timeoutUs);
    const auto maximum =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - start);
    const auto deadline = budget >= maximum ? Clock::time_point::max() : start + budget;
    for (;;)
    {
        {
            // One poller consumes each counter increase. Completion callbacks
            // do not acquire this mutex; never hold their TX locks across SDK IO.
            std::unique_lock<std::timed_mutex> status_lock(tx_status_mutex, std::defer_lock);
            if (!status_lock.try_lock_until(deadline))
                return SOAPY_SDR_TIMEOUT;
            bool pending = false;
            {
                std::scoped_lock lock(tx_mutex, tx_completion->mutex);
                if (tx_writes_stopping)
                    return SOAPY_SDR_STREAM_ERROR;
                pending = first_transmit && !tx_start_finished;
                if (first_transmit && tx_start_finished && tx_start_status != 0)
                    return SOAPY_SDR_STREAM_ERROR;
            }
            if (!pending)
            {
                uint32_t errors = 0;
                const int status =
                    skiq_read_tx_num_underruns(card, stream_handle->tx_handle, &errors);
                if (status != 0)
                {
                    status_lock.unlock();
                    soapy_sidekiq::reportDiagnostic(
                        [&]
                        {
                            SoapySDR_logf(SOAPY_SDR_ERROR,
                                          "skiq_read_tx_num_underruns failed (card %u, handle %u), "
                                          "status %d",
                                          card, stream_handle->tx_handle, status);
                        });
                    return SOAPY_SDR_STREAM_ERROR;
                }
                const bool increased = errors > tx_underruns;
                // Rebase after resets/decreases so future increments remain visible.
                tx_underruns = errors;
                if (increased)
                {
                    chanMask = 1; // Only stream channel zero exists, regardless of SDK handle.
                    return SOAPY_SDR_UNDERFLOW;
                }
            }
        }
        const auto now = Clock::now();
        if (now >= deadline)
            return SOAPY_SDR_TIMEOUT;
        // The SDK exposes a counter, not an event wait. Bound polling and let
        // stopTxWrites wake this call promptly without consuming the timeout.
        std::unique_lock<std::mutex> lock(tx_completion->mutex);
        const auto wake = std::min(deadline, now + std::chrono::milliseconds(1));
        if (tx_completion->changed.wait_until(lock, wake, [this] { return tx_writes_stopping; }))
            return SOAPY_SDR_STREAM_ERROR;
    }
}
