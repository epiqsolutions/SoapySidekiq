#pragma once

#include <SoapySidekiq/RfConfiguration.hpp>

#include <sidekiq_api.h>

#include <cstdint>

namespace soapy_sidekiq
{

class SidekiqRfBackend final : public RfBackend
{
public:
    explicit SidekiqRfBackend(std::uint8_t card) : card_(card) {}

    void writeFrequency(RfDirection, std::uint32_t, std::uint64_t) override;
    std::uint64_t readFrequency(RfDirection, std::uint32_t) override;
    std::pair<std::uint64_t, std::uint64_t> frequencyRange(RfDirection) override;
    void writeRateBandwidth(
        RfDirection, std::uint32_t, std::uint32_t, std::uint32_t) override;
    RateBandwidth readRateBandwidth(RfDirection, std::uint32_t) override;
    std::pair<std::uint32_t, std::uint32_t> sampleRateRange() override;
    void setRxGainAutomatic(std::uint32_t, bool) override;
    bool rxGainAutomatic(std::uint32_t) override;
    std::pair<std::uint8_t, std::uint8_t> rxGainIndexRange(std::uint32_t) override;
    void writeRxGainIndex(std::uint32_t, std::uint8_t) override;
    std::uint8_t readRxGainIndex(std::uint32_t) override;
    void writeTxAttenuation(std::uint32_t, std::uint16_t) override;
    std::uint16_t readTxAttenuation(std::uint32_t) override;

private:
    std::uint8_t card_;
};

GainProfile gainProfileFor(skiq_part_t part);

} // namespace soapy_sidekiq
