/**
 * @file Streaming.cpp
 * @brief Implements shared stream metadata, setup, activation, and deactivation.
 */

#include <vector>
#include <utility>
#include <string>
#include <thread>
#include <mutex>
#include <limits>

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/RxStreamSession.hpp>
#include <exception>
#include <SoapySDR/Formats.hpp>
#include <SoapySidekiq/Diagnostic.hpp>
#include <SoapySidekiq/StreamConfiguration.hpp>
#include <SoapySidekiq/TimestampRate.hpp>
#include <sidekiq_types.h>

namespace
{
/** Translate a SoapySDR direction constant or reject an unknown value. */
soapy_sidekiq::StreamDirection streamDirection(const int direction)
{
    // Reject unknown Soapy constants before selecting RX or TX device state.
    if (direction == SOAPY_SDR_RX)
    {
        return soapy_sidekiq::StreamDirection::rx;
    }
    if (direction == SOAPY_SDR_TX)
    {
        return soapy_sidekiq::StreamDirection::tx;
    }
    throw std::invalid_argument("invalid stream direction " + std::to_string(direction));
}
} // namespace

SoapySidekiq::StreamHandle *SoapySidekiq::findStreamHandle(SoapySDR::Stream *stream) const noexcept
{
    // Compare opaque tokens before dereferencing: null, foreign, and closed
    // handles need not point to accessible memory.
    if (stream == nullptr)
        return nullptr;
    if (stream == reinterpret_cast<SoapySDR::Stream *>(active_rx_stream.get()))
        return active_rx_stream.get();
    if (stream == reinterpret_cast<SoapySDR::Stream *>(active_tx_stream.get()))
        return active_tx_stream.get();
    return nullptr;
}

std::vector<std::string> SoapySidekiq::getStreamFormats(const int direction,
                                                        const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamFormats");

    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CF32);

    return formats;
}

std::string SoapySidekiq::getNativeStreamFormat(const int direction, const size_t channel,
                                                double &fullScale) const
{
    validateChannel(direction, channel);
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNativeStreamFormat");

    const auto stream_direction = streamDirection(direction);
    fullScale =
        stream_direction == soapy_sidekiq::StreamDirection::rx ? this->maxValue : this->txMaxValue;

    return "CS16";
}

SoapySDR::ArgInfoList SoapySidekiq::getStreamArgsInfo(const int direction,
                                                      const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamArgsInfo");
    // setupStream has no per-stream options. Buffer sizes are reported by
    // getStreamMTU; TX payload size is selected at device construction.
    return {};
}

/*******************************************************************
 * Stream API
 ******************************************************************/

SoapySDR::Stream *SoapySidekiq::setupStream(const int direction, const std::string &format,
                                            const std::vector<size_t> &channels,
                                            const SoapySDR::Kwargs &args)
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "setupStream");

    const auto stream_direction = streamDirection(direction);
    const bool already_configured = stream_direction == soapy_sidekiq::StreamDirection::rx
                                        ? active_rx_stream != nullptr
                                        : active_tx_stream != nullptr;
    const std::size_t available_channels =
        stream_direction == soapy_sidekiq::StreamDirection::rx ? num_rx_channels : num_tx_channels;
    const auto request = soapy_sidekiq::validateStreamRequest(
        stream_direction, format, channels, available_channels, already_configured);
    if (!args.empty())
        throw std::invalid_argument("unsupported stream argument '" + args.begin()->first +
                                    "'; no per-stream options are implemented");
    const std::size_t channel = request.channel;

    if (direction == SOAPY_SDR_RX)
    {
        skiq_rx_hdl_t rx_handle = getRxHandle(channel);

        SoapySDR_logf(SOAPY_SDR_INFO, "RX channel %zu uses handle %u", channel, rx_handle);

        status = skiq_read_rx_block_size(card, skiq_rx_stream_mode_balanced);
        if (status < 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_block_size failed: "
                          "card: %u status: %d\n",
                          card, status);
            throw std::runtime_error("skiq_read_rx_block_size failed (card " +
                                     std::to_string(card) + ", status " + std::to_string(status) +
                                     ")");
        }
        const auto block_bytes = static_cast<uint32_t>(status);
        constexpr uint32_t word_bytes = sizeof(uint32_t);
        if (block_bytes <= SKIQ_RX_HEADER_SIZE_IN_BYTES || block_bytes % word_bytes != 0)
            throw std::runtime_error(
                "skiq_read_rx_block_size returned an invalid unpacked block (card " +
                std::to_string(card) + ", bytes " + std::to_string(block_bytes) + ")");
        // Keep setup-only byte sizes local; only the payload sample count is
        // needed by the configured stream's MTU and receive-session methods.
        const auto payload_bytes =
            static_cast<uint32_t>(block_bytes - SKIQ_RX_HEADER_SIZE_IN_BYTES);
        rx_payload_size_in_words = payload_bytes / word_bytes;

        SoapySDR_logf(SOAPY_SDR_INFO, "RX payload size in words: %u", rx_payload_size_in_words);

        rxUseShort = request.format == soapy_sidekiq::StreamFormat::cs16;
        SoapySDR_logf(SOAPY_SDR_INFO, "Using format %s", format.c_str());

        // this has to be called after setting sample rate
        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d", card,
                          status);
            throw std::runtime_error("skiq_read_sys_timestamp_freq failed during RX setup (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", handle " + std::to_string(rx_handle) + ", status " +
                                     std::to_string(status) + ")");
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "System Timestamp Freq: %llu", this->sys_freq);

        if (iq_swap)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR,
                              "skiq_write_iq_order_mode failed (card %u) status %d", card, status);
                throw std::runtime_error(
                    "skiq_write_iq_order_mode failed during RX setup (card " +
                    std::to_string(card) + ", channel " + std::to_string(channel) + ", handle " +
                    std::to_string(rx_handle) + ", IQ order I then Q, status " +
                    std::to_string(status) + ")");
            }
            SoapySDR_logf(SOAPY_SDR_INFO, "RX is set to I then Q order");
        }

        // Bound each blocking receive so the worker can observe its stop request.
        status = skiq_set_rx_transfer_timeout(card, 100000);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_set_rx_transfer_timeout failed, (card %u) status %d", card, status);
            throw std::runtime_error("skiq_set_rx_transfer_timeout failed during RX setup (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", handle " + std::to_string(rx_handle) +
                                     ", timeout 100000 us, status " + std::to_string(status) + ")");
        }

        // Keep the handle locally owned until RX setup is ready to publish it.
        auto stream_handle = std::make_unique<StreamHandle>();
        stream_handle->direction = direction;
        stream_handle->channel = channel;
        stream_handle->rx_handle = rx_handle;
        stream_handle->tx_handle = skiq_tx_hdl_end;
        active_rx_stream = std::move(stream_handle);
        return reinterpret_cast<SoapySDR::Stream *>(active_rx_stream.get());
    }
    else
    {
        skiq_tx_hdl_t tx_handle = getTxHandle(channel);

        SoapySDR_logf(SOAPY_SDR_INFO, "TX channel %zu uses handle %u", channel, tx_handle);

        txUseShort = request.format == soapy_sidekiq::StreamFormat::cs16;
        SoapySDR_logf(SOAPY_SDR_INFO, "Using format %s", format.c_str());

        status = skiq_read_sys_timestamp_freq(this->card, &this->sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed: (card %d), status %d", card,
                          status);
            throw std::runtime_error("skiq_read_sys_timestamp_freq failed during TX setup (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", handle " + std::to_string(tx_handle) + ", status " +
                                     std::to_string(status) + ")");
        }

        // Timestamp setup must succeed before committing TX memory ownership.
        auto stream_handle = std::make_unique<StreamHandle>();
        stream_handle->direction = direction;
        stream_handle->channel = channel;
        stream_handle->rx_handle = skiq_rx_hdl_end;
        stream_handle->tx_handle = tx_handle;
        initializeTxBuffers();

        active_tx_stream = std::move(stream_handle);
        return reinterpret_cast<SoapySDR::Stream *>(active_tx_stream.get());
    }
}

void SoapySidekiq::closeStream(SoapySDR::Stream *stream)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "closeStream");

    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        throw std::invalid_argument("Cannot close an unknown stream handle");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (rx_session != nullptr && rx_session->running())
        {
            throw std::runtime_error("deactivate the RX stream before closing it");
        }

        // Preserve retryable hardware ownership if the backend stop fails.
        if (rx_session != nullptr)
        {
            const int status = deactivateStream(stream, 0, 0);
            if (status != 0)
            {
                throw std::runtime_error("Cannot close RX stream after failed deactivation (card " +
                                         std::to_string(card) + ", status " +
                                         std::to_string(status) + ")");
            }
        }

        if (active_rx_stream.get() == stream_handle)
        {
            active_rx_stream.reset();
        }
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (tx_stream_active)
        {
            throw std::runtime_error("deactivate the TX stream before closing it");
        }

        const int status = stopTxStream(false);
        if (status != 0)
            throw std::runtime_error("Cannot close TX stream after failed deactivation: " +
                                     std::to_string(status));
        active_tx_stream.reset();

        // An accepted SDK submission retains its own block through completion.
        tx_blocks.reset();
    }
}

size_t SoapySidekiq::getStreamMTU(SoapySDR::Stream *stream) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getStreamMTU");

    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        throw std::invalid_argument("Cannot query MTU for an unknown stream handle");

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        return (rx_payload_size_in_words);
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        return current_tx_block_size;
    }
    else
    {
        throw std::invalid_argument("Invalid stream direction for MTU");
    }
}

int SoapySidekiq::activateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs,
                                 const size_t numElems)
{
    try
    {
        return activateStreamImpl(stream, flags, timeNs, numElems);
    }
    catch (const std::exception &error)
    {
        soapy_sidekiq::reportDiagnostic(
            [&] {
                SoapySDR_logf(SOAPY_SDR_ERROR, "Stream activation failed (card %u): %s", card,
                              error.what());
            });
        return SOAPY_SDR_STREAM_ERROR;
    }
}

int SoapySidekiq::activateStreamImpl(SoapySDR::Stream *stream, const int flags,
                                     const long long timeNs, const size_t numElems)
{
    int status = 0;
    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    // Only continuous operation and the legacy next-PPS convention exist.
    // Reject options before any SDK call or asynchronous startup state change.
    if ((flags & ~SOAPY_SDR_HAS_TIME) != 0 || numElems != 0 ||
        ((flags & SOAPY_SDR_HAS_TIME) != 0 && timeNs != 0))
        return SOAPY_SDR_NOT_SUPPORTED;

    soapy_sidekiq::reportDiagnostic([&] { SoapySDR_logf(SOAPY_SDR_TRACE, "activateStream"); });

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (active_rx_stream.get() != stream_handle)
        {
            throw std::runtime_error("RX stream is not the active device RX stream");
        }

        const skiq_rx_hdl_t rx_handle = stream_handle->rx_handle;
        if (rx_session == nullptr)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        if ((flags & SOAPY_SDR_HAS_TIME) != 0)
        {
            bool tx_start_pending;
            {
                std::lock_guard<std::mutex> tx_lock(tx_mutex);
                tx_start_pending = _tx_streaming_thread.joinable() && !tx_start_finished;
            }
            if (tx_start_pending)
            {
                // RX PPS startup may wait behind the outstanding TX start;
                // the delay depends on observed edges, not a fixed duration.
                soapy_sidekiq::reportDiagnostic(
                    [&]
                    {
                        SoapySDR_logf(SOAPY_SDR_WARNING, "RX PPS activation may wait for pending "
                                                         "TX PPS startup and a later edge");
                    });
            }
        }

        // Capture timing and start under the same lock used by coupled rate
        // setters, so configuration cannot change between readback and start.
        status = rx_session->activateConfigured(
            [&]
            {
                uint32_t configured_rate{}, configured_bandwidth{}, actual_bandwidth{};
                double actual_rate = std::numeric_limits<double>::quiet_NaN();
                const int rate_status = skiq_read_rx_sample_rate_and_bandwidth(
                    card, rx_handle, &configured_rate, &actual_rate, &configured_bandwidth,
                    &actual_bandwidth);
                if (rate_status != 0)
                    throw std::runtime_error("RX activation rate read failed (card " +
                                             std::to_string(card) + ", handle " +
                                             std::to_string(rx_handle) +
                                             "): " + std::to_string(rate_status));
                const auto sample_rate = soapy_sidekiq::TimestampRate::fromActualRate(actual_rate);
                uint64_t system_frequency = rfTimeSource ? sys_freq : 0;
                if (!rfTimeSource)
                {
                    const int read_status = skiq_read_sys_timestamp_freq(card, &system_frequency);
                    if (read_status != 0)
                        throw std::runtime_error(
                            "skiq_read_sys_timestamp_freq failed at RX activation: " +
                            std::to_string(read_status));
                }
                return soapy_sidekiq::RxSessionConfiguration{static_cast<uint32_t>(rx_handle),
                                                             rx_payload_size_in_words,
                                                             sample_rate,
                                                             system_frequency,
                                                             rfTimeSource,
                                                             (flags & SOAPY_SDR_HAS_TIME) != 0};
            });
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "starting RX streaming failed (card %u, handle %d), status %d",
                                  card, rx_handle, status);
                });
            throw std::runtime_error("failed to start RX streaming");
        }

        soapy_sidekiq::reportDiagnostic(
            [&] {
                SoapySDR_logf(SOAPY_SDR_INFO, "started receive streaming on handle: %u", rx_handle);
            });
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (active_tx_stream.get() != stream_handle)
        {
            throw std::runtime_error("TX stream is not the active device TX stream");
        }

        // Replacing a joinable PPS thread would terminate the process. A
        // configured/failed start must be deactivated before another attempt.
        if (tx_stream_active || _tx_streaming_thread.joinable())
            return SOAPY_SDR_STREAM_ERROR;

        const skiq_tx_hdl_t tx_handle = stream_handle->tx_handle;

        if (iq_swap)
        {
            status = skiq_write_iq_order_mode(card, skiq_iq_order_iq);
            if (status != 0)
            {
                soapy_sidekiq::reportDiagnostic(
                    [&]
                    {
                        SoapySDR_logf(SOAPY_SDR_ERROR,
                                      "skiq_write_iq_order_mode failed (card %u) status %d", card,
                                      status);
                    });
                return SOAPY_SDR_STREAM_ERROR;
            }
            soapy_sidekiq::reportDiagnostic(
                [&] { SoapySDR_logf(SOAPY_SDR_INFO, "TX is set to I then Q order"); });
        }

        // Constructor validation bounds this payload to the SDK's uint16_t range.
        status =
            skiq_write_tx_block_size(card, tx_handle, static_cast<uint16_t>(current_tx_block_size));
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_write_tx_block_size failed, (card %u) status %d", card,
                                  status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }
        soapy_sidekiq::reportDiagnostic(
            [&] { SoapySDR_logf(SOAPY_SDR_INFO, "TX block size is: %u", current_tx_block_size); });

        //  tx data flow mode
        status = skiq_write_tx_data_flow_mode(card, tx_handle, skiq_tx_immediate_data_flow_mode);
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_write_tx_data_flow_mode failed (card %u) status %d", card,
                                  status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }

        // Use asynchronous transfers with completion callbacks.
        status = skiq_write_tx_transfer_mode(card, tx_handle, skiq_tx_transfer_mode_async);
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_tx_transfer_mode failed, (card %u) status %d", card,
                                  status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }

        // configure 4 threads to be safe, too many and it may consume resources
        status = skiq_write_num_tx_threads(card, 4);
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_write_num_tx_threads failed, (card %u) status %d", card,
                                  status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }

        // A new activation must not inherit a failed PPS start's write gate.
        first_transmit = false;

        if (flags == SOAPY_SDR_HAS_TIME)
        {
            /* The SDK call blocks until the PPS edge. Keep activateStream
             * nonblocking and let the first write wait for the result. */
            {
                std::lock_guard<std::mutex> lock(tx_mutex);
                tx_start_finished = false;
                tx_start_status = 0;
            }
            _tx_streaming_thread = std::thread(&SoapySidekiq::tx_streaming_start, this, tx_handle);

            first_transmit = true;
        }
        else
        {
            status = skiq_start_tx_streaming(card, tx_handle);
            if (status != 0)
            {
                soapy_sidekiq::reportDiagnostic(
                    [&]
                    {
                        SoapySDR_logf(SOAPY_SDR_ERROR,
                                      "skiq_start_tx_streaming failed, (card %u) status %d", card,
                                      status);
                    });
                return SOAPY_SDR_STREAM_ERROR;
            }
            tx_stream_active = true;
            {
                std::lock_guard<std::timed_mutex> lock(tx_status_mutex);
                tx_underruns = 0;
            }
            soapy_sidekiq::reportDiagnostic(
                [&] {
                    SoapySDR_logf(SOAPY_SDR_INFO, "started transmit streaming on handle: %u",
                                  tx_handle);
                });
        }

        tx_stream_active = true;
        {
            std::scoped_lock lock(tx_mutex, tx_completion->mutex);
            tx_writes_stopping = false;
        }
    }

    return 0;
}

int SoapySidekiq::deactivateStream(SoapySDR::Stream *stream, const int flags,
                                   const long long timeNs)
{
    int status = 0;
    StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if ((flags & ~SOAPY_SDR_HAS_TIME) != 0 || ((flags & SOAPY_SDR_HAS_TIME) != 0 && timeNs != 0))
        return SOAPY_SDR_NOT_SUPPORTED;
    soapy_sidekiq::reportDiagnostic([&] { SoapySDR_logf(SOAPY_SDR_TRACE, "deactivateStream"); });

    if (stream_handle->direction == SOAPY_SDR_RX)
    {
        if (active_rx_stream.get() != stream_handle)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        const skiq_rx_hdl_t rx_handle = stream_handle->rx_handle;
        // RxStreamSession joins the worker even when the backend stop fails.
        status = rx_session == nullptr ? SOAPY_SDR_STREAM_ERROR
                                       : rx_session->deactivate((flags & SOAPY_SDR_HAS_TIME) != 0);
        if (status == -ENODEV)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(
                        SOAPY_SDR_WARNING,
                        "RX handle was not streaming while deactivating (card %u, handle %d)", card,
                        rx_handle);
                });
        }
        else if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "stopping RX streaming failed (card %u, handle %d), status %d",
                                  card, rx_handle, status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }
    }
    else if (stream_handle->direction == SOAPY_SDR_TX)
    {
        if (active_tx_stream.get() != stream_handle)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        status = stopTxStream((flags & SOAPY_SDR_HAS_TIME) != 0);
        if (status != 0)
        {
            soapy_sidekiq::reportDiagnostic(
                [&]
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "stopping TX streaming failed (card %u, handle %u), status %d",
                                  card, stream_handle->tx_handle, status);
                });
            return SOAPY_SDR_STREAM_ERROR;
        }
    }

    return 0;
}
