/**
 * @file RxStreaming.cpp
 * @brief Adapts queued native RX samples and timestamps to SoapySDR reads.
 */
#include "SoapySidekiq.hpp"
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

int SoapySidekiq::readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems,
                             int &flags, long long &timeNs, const long timeoutUs)
{
    const StreamHandle *stream_handle = findStreamHandle(stream);
    if (stream_handle == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    if (stream_handle->direction != SOAPY_SDR_RX)
        return SOAPY_SDR_NOT_SUPPORTED;
    if (active_rx_stream.get() != stream_handle)
        return SOAPY_SDR_STREAM_ERROR;
    if (rx_session == nullptr)
        return SOAPY_SDR_STREAM_ERROR;
    flags = 0;
    timeNs = 0;
    if (numElems > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return SOAPY_SDR_OVERFLOW;
    }
    if (timeoutUs < 0 || (numElems != 0 && (buffs == nullptr || buffs[0] == nullptr)))
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    // The queue stores native CS16; CF32 requests use a temporary conversion buffer.
    std::vector<int16_t> converted_samples;
    int16_t *queue_output = nullptr;
    if (numElems != 0)
    {
        if (rxUseShort)
        {
            queue_output = static_cast<int16_t *>(buffs[0]);
        }
        else
        {
            converted_samples.resize(numElems * 2);
            queue_output = converted_samples.data();
        }
    }

    // Session read honors the requested timeout and translates worker lifecycle
    // transitions into stable queue statuses for the SoapySDR API.
    const auto result =
        rx_session->read(queue_output, numElems, std::chrono::microseconds(timeoutUs));
    if (result.status == soapy_sidekiq::RxReadStatus::timeout)
    {
        return SOAPY_SDR_TIMEOUT;
    }
    if (result.status != soapy_sidekiq::RxReadStatus::samples)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (!rxUseShort && result.samples != 0)
    {
        float *output = static_cast<float *>(buffs[0]);
        for (size_t index = 0; index < result.samples * 2; ++index)
        {
            output[index] =
                static_cast<float>(converted_samples[index]) / static_cast<float>(maxValue);
        }
    }
    if (result.has_time)
    {
        flags |= SOAPY_SDR_HAS_TIME;
        timeNs = result.time_ns;
    }
    return static_cast<int>(result.samples);
}
