#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace soapy_sidekiq
{

struct DeviceDescriptor
{
    std::uint8_t card{};
    std::string serial;
    bool available{};
};

struct DiscoveryQuery
{
    std::optional<std::string> card;
    std::optional<std::string> serial;
};

struct DiscoveryError
{
    std::optional<std::uint8_t> card;
    std::string operation;
    std::string message;
};

struct DiscoveryResult
{
    std::vector<DeviceDescriptor> devices;
    std::vector<DiscoveryError> errors;
};

class DiscoveryBackend
{
public:
    virtual ~DiscoveryBackend() = default;

    virtual std::vector<std::uint8_t> cardIds() = 0;
    virtual std::string serial(std::uint8_t card) = 0;
    virtual bool isAvailable(std::uint8_t card) = 0;
};

DiscoveryResult discoverDevices(
    DiscoveryBackend &backend,
    const DiscoveryQuery &query = {});

} // namespace soapy_sidekiq
