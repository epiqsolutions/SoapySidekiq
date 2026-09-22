#pragma once

#include <SoapySidekiq/ChannelMap.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace soapy_sidekiq
{

enum class RfDirection
{
    rx,
    tx
};

enum class GainProfile
{
    legacy,
    x_series,
    nv_series
};

struct RateBandwidth
{
    std::uint32_t configured_rate{};
    double actual_rate{};
    std::uint32_t configured_bandwidth{};
    std::uint32_t actual_bandwidth{};
};

struct NumericRange
{
    double minimum{};
    double maximum{};
    double step{};
};

class RfBackend
{
public:
    virtual ~RfBackend() = default;

    virtual void writeFrequency(
        RfDirection direction, std::uint32_t handle, std::uint64_t frequency) = 0;
    virtual std::uint64_t readFrequency(
        RfDirection direction, std::uint32_t handle) = 0;
    virtual std::pair<std::uint64_t, std::uint64_t> frequencyRange(
        RfDirection direction) = 0;

    virtual void writeRateBandwidth(
        RfDirection direction,
        std::uint32_t handle,
        std::uint32_t rate,
        std::uint32_t bandwidth) = 0;
    virtual RateBandwidth readRateBandwidth(
        RfDirection direction, std::uint32_t handle) = 0;
    virtual std::pair<std::uint32_t, std::uint32_t> sampleRateRange() = 0;

    virtual void setRxGainAutomatic(std::uint32_t handle, bool automatic) = 0;
    virtual bool rxGainAutomatic(std::uint32_t handle) = 0;
    virtual std::pair<std::uint8_t, std::uint8_t> rxGainIndexRange(
        std::uint32_t handle) = 0;
    virtual void writeRxGainIndex(std::uint32_t handle, std::uint8_t index) = 0;
    virtual std::uint8_t readRxGainIndex(std::uint32_t handle) = 0;

    virtual void writeTxAttenuation(std::uint32_t handle, std::uint16_t index) = 0;
    virtual std::uint16_t readTxAttenuation(std::uint32_t handle) = 0;
};

class RfConfiguration
{
public:
    RfConfiguration(
        RfBackend &backend,
        ChannelMap<std::uint32_t, std::uint32_t> channels,
        GainProfile gain_profile,
        std::vector<RateBandwidth> initial_rx,
        std::vector<RateBandwidth> initial_tx,
        std::vector<std::uint16_t> tx_max_attenuation_indices);

    void setFrequency(RfDirection direction, std::size_t channel, double frequency);
    double getFrequency(RfDirection direction, std::size_t channel) const;
    NumericRange getFrequencyRange(RfDirection direction) const;

    void setSampleRate(RfDirection direction, std::size_t channel, double rate);
    double getSampleRate(RfDirection direction, std::size_t channel);
    NumericRange getSampleRateRange() const;

    void setBandwidth(RfDirection direction, std::size_t channel, double bandwidth);
    double getBandwidth(RfDirection direction, std::size_t channel);
    const RateBandwidth &cachedRateBandwidth(
        RfDirection direction, std::size_t channel) const;

    void setGainMode(std::size_t channel, bool automatic);
    bool getGainMode(std::size_t channel) const;
    void setGain(RfDirection direction, std::size_t channel, double value);
    double getGain(RfDirection direction, std::size_t channel) const;
    NumericRange getGainRange(RfDirection direction) const;

private:
    std::uint32_t handle(RfDirection direction, std::size_t channel) const;
    std::vector<RateBandwidth> &states(RfDirection direction);
    const std::vector<RateBandwidth> &states(RfDirection direction) const;
    RateBandwidth refresh(RfDirection direction, std::size_t channel);

    RfBackend &backend_;
    ChannelMap<std::uint32_t, std::uint32_t> channels_;
    GainProfile gain_profile_;
    std::vector<RateBandwidth> rx_;
    std::vector<RateBandwidth> tx_;
    std::vector<std::uint16_t> tx_max_attenuation_indices_;
};

} // namespace soapy_sidekiq
