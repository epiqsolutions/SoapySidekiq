//  Copyright [2018] <Alexander Hurd>"

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/DeviceDiscovery.hpp>
#include <SoapySDR/Registry.hpp>

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

class SidekiqDiscoveryBackend final : public soapy_sidekiq::DiscoveryBackend
{
public:
    std::vector<std::uint8_t> cardIds() override
    {
        std::uint8_t count = 0;
        std::uint8_t cards[SKIQ_MAX_NUM_CARDS]{};
        const int status = skiq_get_cards(skiq_xport_type_auto, &count, cards);
        if (status != 0)
        {
            throw std::runtime_error("skiq_get_cards failed with status " +
                                     std::to_string(status));
        }
        return std::vector<std::uint8_t>(cards, cards + count);
    }

    std::string serial(const std::uint8_t card) override
    {
        char *serial_string = nullptr;
        const int status = skiq_read_serial_string(card, &serial_string);
        if (status != 0 || serial_string == nullptr)
        {
            throw std::runtime_error("skiq_read_serial_string failed with status " +
                                     std::to_string(status));
        }
        return serial_string;
    }

    bool isAvailable(const std::uint8_t card) override
    {
        pid_t owner = 0;
        const int status = skiq_is_card_avail(card, &owner);
        if (status == 0)
        {
            return true;
        }
        if (status == EBUSY)
        {
            return owner == getpid();
        }
        throw std::runtime_error("skiq_is_card_avail failed with status " +
                                 std::to_string(status));
    }
};

soapy_sidekiq::DiscoveryQuery makeQuery(const SoapySDR::Kwargs &args)
{
    soapy_sidekiq::DiscoveryQuery query;
    const auto card = args.find("card");
    if (card != args.end())
    {
        query.card = card->second;
    }
    const auto serial = args.find("serial");
    if (serial != args.end())
    {
        query.serial = serial->second;
    }
    return query;
}

std::vector<SoapySDR::Kwargs> findSidekiq(const SoapySDR::Kwargs &args)
{
    SoapySDR_log(SOAPY_SDR_TRACE, "findSidekiq");
    SidekiqDiscoveryBackend backend;
    const auto discovery = soapy_sidekiq::discoverDevices(backend, makeQuery(args));

    for (const auto &error : discovery.errors)
    {
        if (error.card.has_value())
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "Sidekiq discovery failed for card %u during %s: %s",
                *error.card, error.operation.c_str(), error.message.c_str());
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                "Sidekiq discovery failed during %s: %s",
                error.operation.c_str(), error.message.c_str());
        }
    }

    std::vector<SoapySDR::Kwargs> results;
    results.reserve(discovery.devices.size());
    for (const auto &device : discovery.devices)
    {
        SoapySDR::Kwargs info;
        info["card"] = std::to_string(device.card);
        info["label"] = "Epiq Solutions - Sidekiq :: ";
        info["available"] = device.available ? "Yes" : "No";
        info["product"] = "Sidekiq";
        info["serial"] = device.serial;
        info["manufacturer"] = "Epiq Solutions";
        results.push_back(std::move(info));
    }
    return results;
}

SoapySDR::Device *makeSidekiq(const SoapySDR::Kwargs &args)
{
    return new SoapySidekiq(args);
}

SoapySDR::Registry registerSidekiq(
    "sidekiq", &findSidekiq, &makeSidekiq, SOAPY_SDR_ABI_VERSION);

} // namespace
