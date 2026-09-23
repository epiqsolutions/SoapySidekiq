#include <SoapySidekiq/StreamConfiguration.hpp>

#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{

StreamRequest validateStreamRequest(
    const StreamDirection direction,
    const std::string &format,
    const std::vector<std::size_t> &channels,
    const std::size_t available_channels,
    const bool stream_already_configured)
{
    if (stream_already_configured)
    {
        throw std::logic_error(
            direction == StreamDirection::rx
                ? "only one RX stream per device is currently supported"
                : "only one TX stream per device is currently supported");
    }
    if (channels.size() > 1)
    {
        throw std::invalid_argument(
            direction == StreamDirection::rx
                ? "multi-channel RX streams are not currently supported"
                : "multi-channel TX streams are not currently supported");
    }

    const std::size_t channel = channels.empty() ? 0 : channels.front();
    if (channel >= available_channels)
    {
        throw std::out_of_range(
            std::string{direction == StreamDirection::rx ? "RX" : "TX"} +
            " stream channel " + std::to_string(channel) +
            " is unavailable; device has " + std::to_string(available_channels) +
            " channel(s)");
    }

    StreamFormat parsed_format;
    if (format == "CS16")
    {
        parsed_format = StreamFormat::cs16;
    }
    else if (format == "CF32")
    {
        parsed_format = StreamFormat::cf32;
    }
    else
    {
        throw std::invalid_argument(
            "unsupported stream format '" + format + "'; expected CS16 or CF32");
    }

    return {direction, parsed_format, channel};
}

} // namespace soapy_sidekiq
