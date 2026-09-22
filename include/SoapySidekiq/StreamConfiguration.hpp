#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soapy_sidekiq
{

enum class StreamDirection
{
    rx,
    tx
};

enum class StreamFormat
{
    cs16,
    cf32
};

struct StreamRequest
{
    StreamDirection direction;
    StreamFormat format;
    std::size_t channel;
};

StreamRequest validateStreamRequest(
    StreamDirection direction,
    const std::string &format,
    const std::vector<std::size_t> &channels,
    std::size_t available_channels,
    bool stream_already_configured);

} // namespace soapy_sidekiq
