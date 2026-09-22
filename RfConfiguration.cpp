#include <SoapySidekiq/RfConfiguration.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{
namespace
{

template <typename Integer>
Integer checkedUnsigned(const double value, const char *name)
{
    if (!std::isfinite(value) || value < 0.0 ||
        static_cast<long double>(value) >
            static_cast<long double>(std::numeric_limits<Integer>::max()))
    {
        throw std::invalid_argument(std::string{name} + " is outside the supported range");
    }
    return static_cast<Integer>(value);
}

struct RxGainScale
{
    int index_offset;
    double indices_per_db;
    double maximum;
};

RxGainScale rxGainScale(const GainProfile profile)
{
    switch (profile)
    {
    case GainProfile::legacy:
        return {0, 1.0, 76.0};
    case GainProfile::x_series:
        return {195, 2.0, 30.0};
    case GainProfile::nv_series:
        return {187, 2.0, 34.0};
    }
    throw std::logic_error("unsupported gain profile");
}

double txMaximum(const GainProfile profile)
{
    return profile == GainProfile::legacy ? 89.75 : 41.75;
}

} // namespace

RfConfiguration::RfConfiguration(
    RfBackend &backend,
    ChannelMap<std::uint32_t, std::uint32_t> channels,
    const GainProfile gain_profile,
    std::vector<RateBandwidth> initial_rx,
    std::vector<RateBandwidth> initial_tx,
    std::vector<std::uint16_t> tx_max_attenuation_indices)
    : backend_(backend),
      channels_(std::move(channels)),
      gain_profile_(gain_profile),
      rx_(std::move(initial_rx)),
      tx_(std::move(initial_tx)),
      tx_max_attenuation_indices_(std::move(tx_max_attenuation_indices))
{
    if (rx_.size() != channels_.rxCount())
    {
        throw std::invalid_argument("RX state count does not match RX channel count");
    }
    if (tx_.size() != channels_.txCount())
    {
        throw std::invalid_argument("TX state count does not match TX channel count");
    }
    if (tx_max_attenuation_indices_.size() != channels_.txCount())
    {
        throw std::invalid_argument("TX attenuation count does not match TX channel count");
    }
    const auto validate_states = [](const std::vector<RateBandwidth> &values) {
        for (const auto &value : values)
        {
            if (!std::isfinite(value.actual_rate) || value.actual_rate < 0.0 ||
                static_cast<long double>(value.actual_rate) >
                    static_cast<long double>(std::numeric_limits<std::uint32_t>::max()))
            {
                throw std::invalid_argument("initial actual sample rate is invalid");
            }
        }
    };
    validate_states(rx_);
    validate_states(tx_);
}

std::uint32_t RfConfiguration::handle(
    const RfDirection direction, const std::size_t channel) const
{
    return direction == RfDirection::rx
        ? channels_.rxHandle(channel)
        : channels_.txHandle(channel);
}

std::vector<RateBandwidth> &RfConfiguration::states(const RfDirection direction)
{
    return direction == RfDirection::rx ? rx_ : tx_;
}

const std::vector<RateBandwidth> &RfConfiguration::states(
    const RfDirection direction) const
{
    return direction == RfDirection::rx ? rx_ : tx_;
}

void RfConfiguration::setFrequency(
    const RfDirection direction, const std::size_t channel, const double frequency)
{
    backend_.writeFrequency(
        direction, handle(direction, channel),
        checkedUnsigned<std::uint64_t>(frequency, "frequency"));
}

double RfConfiguration::getFrequency(
    const RfDirection direction, const std::size_t channel) const
{
    return static_cast<double>(backend_.readFrequency(direction, handle(direction, channel)));
}

NumericRange RfConfiguration::getFrequencyRange(const RfDirection direction) const
{
    const auto range = backend_.frequencyRange(direction);
    if (range.first > range.second)
    {
        throw std::runtime_error("backend returned an invalid frequency range");
    }
    return {static_cast<double>(range.first), static_cast<double>(range.second), 0.0};
}

RateBandwidth RfConfiguration::refresh(
    const RfDirection direction, const std::size_t channel)
{
    RateBandwidth value = backend_.readRateBandwidth(direction, handle(direction, channel));
    if (!std::isfinite(value.actual_rate) || value.actual_rate < 0.0 ||
        static_cast<long double>(value.actual_rate) >
            static_cast<long double>(std::numeric_limits<std::uint32_t>::max()))
    {
        throw std::runtime_error("backend returned an invalid actual sample rate");
    }
    states(direction).at(channel) = value;
    return value;
}

void RfConfiguration::setSampleRate(
    const RfDirection direction, const std::size_t channel, const double rate)
{
    const std::uint32_t requested_rate = checkedUnsigned<std::uint32_t>(rate, "sample rate");
    const std::uint32_t requested_bandwidth =
        std::min(states(direction).at(channel).actual_bandwidth, requested_rate);
    backend_.writeRateBandwidth(
        direction, handle(direction, channel), requested_rate, requested_bandwidth);
    refresh(direction, channel);
}

double RfConfiguration::getSampleRate(
    const RfDirection direction, const std::size_t channel)
{
    return static_cast<double>(refresh(direction, channel).configured_rate);
}

NumericRange RfConfiguration::getSampleRateRange() const
{
    const auto range = backend_.sampleRateRange();
    if (range.first > range.second)
    {
        throw std::runtime_error("backend returned an invalid sample-rate range");
    }
    return {static_cast<double>(range.first), static_cast<double>(range.second), 0.0};
}

void RfConfiguration::setBandwidth(
    const RfDirection direction, const std::size_t channel, const double bandwidth)
{
    const std::uint32_t requested = checkedUnsigned<std::uint32_t>(bandwidth, "bandwidth");
    backend_.writeRateBandwidth(
        direction,
        handle(direction, channel),
        static_cast<std::uint32_t>(states(direction).at(channel).actual_rate),
        requested);
    refresh(direction, channel);
}

double RfConfiguration::getBandwidth(
    const RfDirection direction, const std::size_t channel)
{
    return static_cast<double>(refresh(direction, channel).actual_bandwidth);
}

const RateBandwidth &RfConfiguration::cachedRateBandwidth(
    const RfDirection direction, const std::size_t channel) const
{
    return states(direction).at(channel);
}

void RfConfiguration::setGainMode(const std::size_t channel, const bool automatic)
{
    backend_.setRxGainAutomatic(channels_.rxHandle(channel), automatic);
}

bool RfConfiguration::getGainMode(const std::size_t channel) const
{
    return backend_.rxGainAutomatic(channels_.rxHandle(channel));
}

void RfConfiguration::setGain(
    const RfDirection direction, const std::size_t channel, const double value)
{
    if (!std::isfinite(value))
    {
        throw std::invalid_argument("gain must be finite");
    }

    if (direction == RfDirection::rx)
    {
        const auto scale = rxGainScale(gain_profile_);
        if (value < 0.0 || value > scale.maximum)
        {
            throw std::invalid_argument("RX gain is outside the supported range");
        }
        const std::uint32_t rx_handle = channels_.rxHandle(channel);
        if (backend_.rxGainAutomatic(rx_handle))
        {
            backend_.setRxGainAutomatic(rx_handle, false);
        }
        const auto hardware_range = backend_.rxGainIndexRange(rx_handle);
        if (hardware_range.first > hardware_range.second)
        {
            throw std::runtime_error("backend returned an invalid RX gain-index range");
        }
        const int requested = scale.index_offset +
            static_cast<int>(std::round(value * scale.indices_per_db));
        const int clamped = std::clamp(
            requested,
            static_cast<int>(hardware_range.first),
            static_cast<int>(hardware_range.second));
        backend_.writeRxGainIndex(rx_handle, static_cast<std::uint8_t>(clamped));
        return;
    }

    const double maximum = txMaximum(gain_profile_);
    if (value < 0.0 || value > maximum)
    {
        throw std::invalid_argument("TX gain is outside the supported range");
    }
    const std::uint32_t requested_steps = static_cast<std::uint32_t>(std::round(value * 4.0));
    const std::uint16_t maximum_index = tx_max_attenuation_indices_.at(channel);
    if (requested_steps > maximum_index)
    {
        throw std::invalid_argument("TX gain exceeds the hardware attenuation range");
    }
    backend_.writeTxAttenuation(
        channels_.txHandle(channel),
        static_cast<std::uint16_t>(maximum_index - requested_steps));
}

double RfConfiguration::getGain(
    const RfDirection direction, const std::size_t channel) const
{
    if (direction == RfDirection::rx)
    {
        const auto scale = rxGainScale(gain_profile_);
        const int index = static_cast<int>(backend_.readRxGainIndex(channels_.rxHandle(channel)));
        return static_cast<double>(index - scale.index_offset) / scale.indices_per_db;
    }

    const std::uint16_t maximum_index = tx_max_attenuation_indices_.at(channel);
    const std::uint16_t index = backend_.readTxAttenuation(channels_.txHandle(channel));
    if (index > maximum_index)
    {
        throw std::runtime_error("backend returned an invalid TX attenuation index");
    }
    return static_cast<double>(maximum_index - index) / 4.0;
}

NumericRange RfConfiguration::getGainRange(const RfDirection direction) const
{
    if (direction == RfDirection::rx)
    {
        const auto scale = rxGainScale(gain_profile_);
        return {0.0, scale.maximum, 1.0 / scale.indices_per_db};
    }
    return {0.0, txMaximum(gain_profile_), 0.25};
}

} // namespace soapy_sidekiq
