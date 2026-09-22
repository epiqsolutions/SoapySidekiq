#include "SidekiqRfBackend.hpp"

#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{
namespace
{

void checkStatus(const int status, const char *operation)
{
    if (status != 0)
    {
        throw std::runtime_error(
            std::string{operation} + " failed with status " + std::to_string(status));
    }
}

skiq_rx_hdl_t rxHandle(const std::uint32_t handle)
{
    return static_cast<skiq_rx_hdl_t>(handle);
}

skiq_tx_hdl_t txHandle(const std::uint32_t handle)
{
    return static_cast<skiq_tx_hdl_t>(handle);
}

} // namespace

void SidekiqRfBackend::writeFrequency(
    const RfDirection direction,
    const std::uint32_t handle,
    const std::uint64_t frequency)
{
    const int status = direction == RfDirection::rx
        ? skiq_write_rx_LO_freq(card_, rxHandle(handle), frequency)
        : skiq_write_tx_LO_freq(card_, txHandle(handle), frequency);
    checkStatus(status, direction == RfDirection::rx
        ? "skiq_write_rx_LO_freq" : "skiq_write_tx_LO_freq");
}

std::uint64_t SidekiqRfBackend::readFrequency(
    const RfDirection direction, const std::uint32_t handle)
{
    std::uint64_t frequency = 0;
    double tuned_frequency = 0.0;
    const int status = direction == RfDirection::rx
        ? skiq_read_rx_LO_freq(card_, rxHandle(handle), &frequency, &tuned_frequency)
        : skiq_read_tx_LO_freq(card_, txHandle(handle), &frequency, &tuned_frequency);
    checkStatus(status, direction == RfDirection::rx
        ? "skiq_read_rx_LO_freq" : "skiq_read_tx_LO_freq");
    return frequency;
}

std::pair<std::uint64_t, std::uint64_t> SidekiqRfBackend::frequencyRange(
    const RfDirection direction)
{
    std::uint64_t maximum = 0;
    std::uint64_t minimum = 0;
    const int status = direction == RfDirection::rx
        ? skiq_read_rx_LO_freq_range(card_, &maximum, &minimum)
        : skiq_read_tx_LO_freq_range(card_, &maximum, &minimum);
    checkStatus(status, direction == RfDirection::rx
        ? "skiq_read_rx_LO_freq_range" : "skiq_read_tx_LO_freq_range");
    return {minimum, maximum};
}

void SidekiqRfBackend::writeRateBandwidth(
    const RfDirection direction,
    const std::uint32_t handle,
    const std::uint32_t rate,
    const std::uint32_t bandwidth)
{
    const int status = direction == RfDirection::rx
        ? skiq_write_rx_sample_rate_and_bandwidth(
              card_, rxHandle(handle), rate, bandwidth)
        : skiq_write_tx_sample_rate_and_bandwidth(
              card_, txHandle(handle), rate, bandwidth);
    checkStatus(status, direction == RfDirection::rx
        ? "skiq_write_rx_sample_rate_and_bandwidth"
        : "skiq_write_tx_sample_rate_and_bandwidth");
}

RateBandwidth SidekiqRfBackend::readRateBandwidth(
    const RfDirection direction, const std::uint32_t handle)
{
    RateBandwidth result;
    const int status = direction == RfDirection::rx
        ? skiq_read_rx_sample_rate_and_bandwidth(
              card_, rxHandle(handle),
              &result.configured_rate, &result.actual_rate,
              &result.configured_bandwidth, &result.actual_bandwidth)
        : skiq_read_tx_sample_rate_and_bandwidth(
              card_, txHandle(handle),
              &result.configured_rate, &result.actual_rate,
              &result.configured_bandwidth, &result.actual_bandwidth);
    checkStatus(status, direction == RfDirection::rx
        ? "skiq_read_rx_sample_rate_and_bandwidth"
        : "skiq_read_tx_sample_rate_and_bandwidth");
    return result;
}

std::pair<std::uint32_t, std::uint32_t> SidekiqRfBackend::sampleRateRange()
{
    std::uint32_t minimum = 0;
    std::uint32_t maximum = 0;
    checkStatus(skiq_read_min_sample_rate(card_, &minimum), "skiq_read_min_sample_rate");
    checkStatus(skiq_read_max_sample_rate(card_, &maximum), "skiq_read_max_sample_rate");
    return {minimum, maximum};
}

void SidekiqRfBackend::setRxGainAutomatic(
    const std::uint32_t handle, const bool automatic)
{
    checkStatus(
        skiq_write_rx_gain_mode(
            card_, rxHandle(handle), automatic ? skiq_rx_gain_auto : skiq_rx_gain_manual),
        "skiq_write_rx_gain_mode");
}

bool SidekiqRfBackend::rxGainAutomatic(const std::uint32_t handle)
{
    skiq_rx_gain_t mode = skiq_rx_gain_manual;
    checkStatus(
        skiq_read_rx_gain_mode(card_, rxHandle(handle), &mode),
        "skiq_read_rx_gain_mode");
    return mode == skiq_rx_gain_auto;
}

std::pair<std::uint8_t, std::uint8_t> SidekiqRfBackend::rxGainIndexRange(
    const std::uint32_t handle)
{
    std::uint8_t minimum = 0;
    std::uint8_t maximum = 0;
    checkStatus(
        skiq_read_rx_gain_index_range(card_, rxHandle(handle), &minimum, &maximum),
        "skiq_read_rx_gain_index_range");
    return {minimum, maximum};
}

void SidekiqRfBackend::writeRxGainIndex(
    const std::uint32_t handle, const std::uint8_t index)
{
    checkStatus(
        skiq_write_rx_gain(card_, rxHandle(handle), index),
        "skiq_write_rx_gain");
}

std::uint8_t SidekiqRfBackend::readRxGainIndex(const std::uint32_t handle)
{
    std::uint8_t index = 0;
    checkStatus(skiq_read_rx_gain(card_, rxHandle(handle), &index), "skiq_read_rx_gain");
    return index;
}

void SidekiqRfBackend::writeTxAttenuation(
    const std::uint32_t handle, const std::uint16_t index)
{
    checkStatus(
        skiq_write_tx_attenuation(card_, txHandle(handle), index),
        "skiq_write_tx_attenuation");
}

std::uint16_t SidekiqRfBackend::readTxAttenuation(const std::uint32_t handle)
{
    std::uint16_t index = 0;
    checkStatus(
        skiq_read_tx_attenuation(card_, txHandle(handle), &index),
        "skiq_read_tx_attenuation");
    return index;
}

GainProfile gainProfileFor(const skiq_part_t part)
{
    switch (part)
    {
    case skiq_mpcie:
    case skiq_m2:
    case skiq_m2_2280:
    case skiq_z2:
    case skiq_z3u:
        return GainProfile::legacy;
    case skiq_x2:
    case skiq_x4:
    case skiq_x40:
        return GainProfile::x_series;
    case skiq_nv100:
    case skiq_nvm2:
    case skiq_z4:
    case skiq_z4_mp:
        return GainProfile::nv_series;
    default:
        throw std::runtime_error("unsupported Sidekiq part for gain conversion");
    }
}

} // namespace soapy_sidekiq
