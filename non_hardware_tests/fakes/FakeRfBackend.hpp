#pragma once

#include <SoapySidekiq/RfConfiguration.hpp>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

class FakeRfBackend final : public soapy_sidekiq::RfBackend
{
public:
    using Direction = soapy_sidekiq::RfDirection;
    using Key = std::pair<Direction, std::uint32_t>;

    struct RateWrite
    {
        Direction direction{};
        std::uint32_t handle{};
        std::uint32_t rate{};
        std::uint32_t bandwidth{};
    };

    void writeFrequency(
        const Direction direction,
        const std::uint32_t handle,
        const std::uint64_t frequency) override
    {
        failIfRequested();
        last_frequency_write = std::make_tuple(direction, handle, frequency);
        frequencies[{direction, handle}] = frequency;
    }

    std::uint64_t readFrequency(
        const Direction direction, const std::uint32_t handle) override
    {
        failIfRequested();
        return frequencies.at({direction, handle});
    }

    std::pair<std::uint64_t, std::uint64_t> frequencyRange(
        const Direction direction) override
    {
        failIfRequested();
        return direction == Direction::rx ? rx_frequency_range : tx_frequency_range;
    }

    void writeRateBandwidth(
        const Direction direction,
        const std::uint32_t handle,
        const std::uint32_t rate,
        const std::uint32_t bandwidth) override
    {
        failIfRequested();
        last_rate_write = {direction, handle, rate, bandwidth};
    }

    soapy_sidekiq::RateBandwidth readRateBandwidth(
        const Direction direction, const std::uint32_t handle) override
    {
        failIfRequested();
        return rate_bandwidth.at({direction, handle});
    }

    std::pair<std::uint32_t, std::uint32_t> sampleRateRange() override
    {
        failIfRequested();
        return sample_rate_range;
    }

    void setRxGainAutomatic(const std::uint32_t handle, const bool automatic) override
    {
        failIfRequested();
        gain_automatic[handle] = automatic;
        ++gain_mode_write_count;
    }

    bool rxGainAutomatic(const std::uint32_t handle) override
    {
        failIfRequested();
        return gain_automatic.at(handle);
    }

    std::pair<std::uint8_t, std::uint8_t> rxGainIndexRange(
        const std::uint32_t handle) override
    {
        failIfRequested();
        return rx_gain_ranges.at(handle);
    }

    void writeRxGainIndex(const std::uint32_t handle, const std::uint8_t index) override
    {
        failIfRequested();
        rx_gain_indices[handle] = index;
    }

    std::uint8_t readRxGainIndex(const std::uint32_t handle) override
    {
        failIfRequested();
        return rx_gain_indices.at(handle);
    }

    void writeTxAttenuation(const std::uint32_t handle, const std::uint16_t index) override
    {
        failIfRequested();
        tx_attenuation_indices[handle] = index;
    }

    std::uint16_t readTxAttenuation(const std::uint32_t handle) override
    {
        failIfRequested();
        return tx_attenuation_indices.at(handle);
    }

    void failNextCall()
    {
        fail_next_call = true;
    }

    std::map<Key, std::uint64_t> frequencies;
    std::map<Key, soapy_sidekiq::RateBandwidth> rate_bandwidth;
    std::map<std::uint32_t, bool> gain_automatic;
    std::map<std::uint32_t, std::pair<std::uint8_t, std::uint8_t>> rx_gain_ranges;
    std::map<std::uint32_t, std::uint8_t> rx_gain_indices;
    std::map<std::uint32_t, std::uint16_t> tx_attenuation_indices;
    std::pair<std::uint64_t, std::uint64_t> rx_frequency_range{70'000'000, 6'000'000'000};
    std::pair<std::uint64_t, std::uint64_t> tx_frequency_range{75'000'000, 6'000'000'000};
    std::pair<std::uint32_t, std::uint32_t> sample_rate_range{40'000, 122'880'000};
    std::tuple<Direction, std::uint32_t, std::uint64_t> last_frequency_write{};
    RateWrite last_rate_write{};
    std::size_t gain_mode_write_count{};

private:
    void failIfRequested()
    {
        if (fail_next_call)
        {
            fail_next_call = false;
            throw std::runtime_error("scripted RF backend failure");
        }
    }

    bool fail_next_call{};
};
