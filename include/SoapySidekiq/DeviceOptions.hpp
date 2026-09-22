#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace soapy_sidekiq
{

inline constexpr std::uint32_t defaultTxBlockSize = 16380;

struct DeviceOptions
{
    std::uint8_t card{};
    std::optional<std::uint8_t> topology;
    std::uint32_t tx_block_size{defaultTxBlockSize};
    std::optional<std::string> clock_source;
    std::optional<std::string> time_source;
};

DeviceOptions parseDeviceOptions(const std::map<std::string, std::string> &arguments);

} // namespace soapy_sidekiq
