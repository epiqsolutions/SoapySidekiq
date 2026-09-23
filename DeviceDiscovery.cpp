#include <SoapySidekiq/DeviceDiscovery.hpp>

#include <exception>
#include <string>
#include <utility>

namespace soapy_sidekiq
{
namespace
{

bool matchesQuery(const DeviceDescriptor &device, const DiscoveryQuery &query)
{
    if (query.card.has_value())
    {
        return *query.card == std::to_string(device.card);
    }
    if (query.serial.has_value())
    {
        return *query.serial == device.serial;
    }
    return true;
}

std::string exceptionMessage(const std::exception &error)
{
    const std::string message = error.what();
    return message.empty() ? "unknown error" : message;
}

} // namespace

DiscoveryResult discoverDevices(DiscoveryBackend &backend, const DiscoveryQuery &query)
{
    DiscoveryResult result;
    std::vector<std::uint8_t> card_ids;
    try
    {
        card_ids = backend.cardIds();
    }
    catch (const std::exception &error)
    {
        result.errors.push_back(
            {std::nullopt, "enumerate cards", exceptionMessage(error)});
        return result;
    }

    for (const std::uint8_t card : card_ids)
    {
        DeviceDescriptor device;
        device.card = card;
        try
        {
            device.serial = backend.serial(card);
        }
        catch (const std::exception &error)
        {
            result.errors.push_back(
                {card, "read serial", exceptionMessage(error)});
            continue;
        }

        try
        {
            device.available = backend.isAvailable(card);
        }
        catch (const std::exception &error)
        {
            result.errors.push_back(
                {card, "check availability", exceptionMessage(error)});
            device.available = false;
        }

        if (matchesQuery(device, query))
        {
            result.devices.push_back(std::move(device));
        }
    }
    return result;
}

} // namespace soapy_sidekiq
