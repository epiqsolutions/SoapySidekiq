#include <SoapySidekiq/DeviceOptions.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace soapy_sidekiq
{
namespace
{

std::uint32_t parseUnsigned(
    const std::string &value,
    const std::string &name,
    const std::uint32_t maximum)
{
    std::uint32_t parsed = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto conversion = std::from_chars(begin, end, parsed);
    if (value.empty() || conversion.ec != std::errc{} || conversion.ptr != end ||
        parsed > maximum)
    {
        throw std::invalid_argument(
            "invalid " + name + " value '" + value + "'");
    }
    return parsed;
}

std::optional<std::string> optionalString(
    const std::map<std::string, std::string> &arguments,
    const std::string &key)
{
    const auto value = arguments.find(key);
    if (value == arguments.end())
    {
        return std::nullopt;
    }
    if (value->second.empty())
    {
        throw std::invalid_argument(key + " cannot be empty");
    }
    return value->second;
}

} // namespace

DeviceOptions parseDeviceOptions(const std::map<std::string, std::string> &arguments)
{
    const auto card = arguments.find("card");
    if (card == arguments.end())
    {
        throw std::invalid_argument("the card argument is required");
    }

    DeviceOptions options;
    options.card = static_cast<std::uint8_t>(
        parseUnsigned(card->second, "card", std::numeric_limits<std::uint8_t>::max()));

    const auto topology = arguments.find("topology");
    if (topology != arguments.end())
    {
        options.topology = static_cast<std::uint8_t>(
            parseUnsigned(topology->second,
                          "topology",
                          std::numeric_limits<std::uint8_t>::max()));
    }

    const auto tx_block_size = arguments.find("tx_block_size");
    if (tx_block_size != arguments.end())
    {
        options.tx_block_size = parseUnsigned(
            tx_block_size->second,
            "tx_block_size",
            std::numeric_limits<std::uint16_t>::max());
        if (options.tx_block_size == 0)
        {
            throw std::invalid_argument("tx_block_size must be greater than zero");
        }
    }

    options.clock_source = optionalString(arguments, "clock_source");
    options.time_source = optionalString(arguments, "time_source");
    return options;
}

} // namespace soapy_sidekiq
