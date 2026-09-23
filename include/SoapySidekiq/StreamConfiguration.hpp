#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soapy_sidekiq
{

/** Direction of a validated stream request. */
enum class StreamDirection
{
    rx,
    tx
};

/** Sample formats implemented by both RX and TX stream paths. */
enum class StreamFormat
{
    cs16,
    cf32
};

/** Normalized stream settings safe to use for hardware configuration. */
struct StreamRequest
{
    /** Requested receive or transmit direction. */
    StreamDirection direction;
    /** Parsed sample format. */
    StreamFormat format;
    /** Single logical channel selected for this stream. */
    std::size_t channel;
};

/**
 * Validate and normalize a Soapy stream request before hardware side effects.
 *
 * An omitted channel selects channel zero. Only one channel and one stream per
 * direction are currently supported.
 */
StreamRequest validateStreamRequest(
    StreamDirection direction,
    const std::string &format,
    const std::vector<std::size_t> &channels,
    std::size_t available_channels,
    bool stream_already_configured);

} // namespace soapy_sidekiq
