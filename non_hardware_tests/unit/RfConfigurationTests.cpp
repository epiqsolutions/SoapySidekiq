#include "FakeRfBackend.hpp"
#include "TestHarness.hpp"

#include <SoapySidekiq/ChannelMap.hpp>
#include <SoapySidekiq/RfConfiguration.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using soapy_sidekiq::ChannelMap;
using soapy_sidekiq::GainProfile;
using soapy_sidekiq::RateBandwidth;
using soapy_sidekiq::RfConfiguration;
using soapy_sidekiq::RfDirection;

namespace
{

RfConfiguration makeConfiguration(
    FakeRfBackend &backend, const GainProfile profile = GainProfile::x_series)
{
    backend.frequencies[{RfDirection::rx, 11}] = 100'000'000;
    backend.frequencies[{RfDirection::tx, 22}] = 200'000'000;
    backend.rate_bandwidth[{RfDirection::rx, 11}] =
        {10'000'000, 9'999'999.5, 8'000'000, 7'900'000};
    backend.rate_bandwidth[{RfDirection::tx, 22}] =
        {20'000'000, 19'999'999.5, 15'000'000, 14'900'000};
    backend.gain_automatic[11] = false;
    backend.rx_gain_ranges[11] = {195, 255};
    backend.rx_gain_indices[11] = 195;
    backend.tx_attenuation_indices[22] = 167;

    return RfConfiguration(
        backend,
        ChannelMap<std::uint32_t, std::uint32_t>({11}, {22}),
        profile,
        {{10'000'000, 10'000'000.0, 8'000'000, 8'000'000}},
        {{20'000'000, 20'000'000.0, 15'000'000, 15'000'000}},
        {167});
}

} // namespace

TEST_CASE("RF configuration routes RX and TX frequency operations through mapped handles")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);

    configuration.setFrequency(RfDirection::rx, 0, 915'000'000.0);
    REQUIRE(std::get<0>(backend.last_frequency_write) == RfDirection::rx);
    REQUIRE_EQ(std::get<1>(backend.last_frequency_write), std::uint32_t{11});
    REQUIRE_EQ(std::get<2>(backend.last_frequency_write), std::uint64_t{915'000'000});

    configuration.setFrequency(RfDirection::tx, 0, 2'450'000'000.0);
    REQUIRE(std::get<0>(backend.last_frequency_write) == RfDirection::tx);
    REQUIRE_EQ(std::get<1>(backend.last_frequency_write), std::uint32_t{22});
    REQUIRE_EQ(configuration.getFrequency(RfDirection::tx, 0), 2'450'000'000.0);
}

TEST_CASE("RF configuration exposes backend frequency and sample-rate ranges")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);

    const auto rx_frequency = configuration.getFrequencyRange(RfDirection::rx);
    REQUIRE_EQ(rx_frequency.minimum, 70'000'000.0);
    REQUIRE_EQ(rx_frequency.maximum, 6'000'000'000.0);
    const auto sample_rate = configuration.getSampleRateRange();
    REQUIRE_EQ(sample_rate.minimum, 40'000.0);
    REQUIRE_EQ(sample_rate.maximum, 122'880'000.0);
}

TEST_CASE("setting sample rate caps the prior bandwidth and caches backend readback")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    backend.rate_bandwidth[{RfDirection::rx, 11}] =
        {5'000'000, 4'999'999.0, 5'000'000, 4'800'000};

    configuration.setSampleRate(RfDirection::rx, 0, 5'000'000.0);

    REQUIRE(backend.last_rate_write.direction == RfDirection::rx);
    REQUIRE_EQ(backend.last_rate_write.handle, std::uint32_t{11});
    REQUIRE_EQ(backend.last_rate_write.rate, std::uint32_t{5'000'000});
    REQUIRE_EQ(backend.last_rate_write.bandwidth, std::uint32_t{5'000'000});
    const auto &cached = configuration.cachedRateBandwidth(RfDirection::rx, 0);
    REQUIRE_EQ(cached.actual_rate, 4'999'999.0);
    REQUIRE_EQ(cached.actual_bandwidth, std::uint32_t{4'800'000});
}

TEST_CASE("setting bandwidth preserves the cached actual sample rate")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    backend.rate_bandwidth[{RfDirection::tx, 22}] =
        {19'999'999, 19'999'998.5, 12'000'000, 11'900'000};

    configuration.setBandwidth(RfDirection::tx, 0, 12'000'000.0);

    REQUIRE(backend.last_rate_write.direction == RfDirection::tx);
    REQUIRE_EQ(backend.last_rate_write.handle, std::uint32_t{22});
    REQUIRE_EQ(backend.last_rate_write.rate, std::uint32_t{20'000'000});
    REQUIRE_EQ(backend.last_rate_write.bandwidth, std::uint32_t{12'000'000});
    REQUIRE_EQ(configuration.getBandwidth(RfDirection::tx, 0), 11'900'000.0);
}

TEST_CASE("explicit RX gain disables automatic mode and uses profile conversion")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    backend.gain_automatic[11] = true;

    configuration.setGain(RfDirection::rx, 0, 12.5);

    REQUIRE(!backend.gain_automatic[11]);
    REQUIRE_EQ(backend.gain_mode_write_count, std::size_t{1});
    REQUIRE_EQ(backend.rx_gain_indices[11], std::uint8_t{220});
    REQUIRE_EQ(configuration.getGain(RfDirection::rx, 0), 12.5);
}

TEST_CASE("RX gain conversion supports every hardware profile")
{
    FakeRfBackend legacy_backend;
    auto legacy = makeConfiguration(legacy_backend, GainProfile::legacy);
    legacy_backend.rx_gain_ranges[11] = {0, 76};
    legacy.setGain(RfDirection::rx, 0, 31.6);
    REQUIRE_EQ(legacy_backend.rx_gain_indices[11], std::uint8_t{32});
    REQUIRE_EQ(legacy.getGainRange(RfDirection::rx).step, 1.0);

    FakeRfBackend nv_backend;
    auto nv = makeConfiguration(nv_backend, GainProfile::nv_series);
    nv_backend.rx_gain_ranges[11] = {187, 255};
    nv.setGain(RfDirection::rx, 0, 21.25);
    REQUIRE_EQ(nv_backend.rx_gain_indices[11], std::uint8_t{230});
    REQUIRE_EQ(nv.getGain(RfDirection::rx, 0), 21.5);
}

TEST_CASE("RX gain is clamped to the hardware-reported index range")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    backend.rx_gain_ranges[11] = {200, 230};

    configuration.setGain(RfDirection::rx, 0, 0.0);
    REQUIRE_EQ(backend.rx_gain_indices[11], std::uint8_t{200});
    configuration.setGain(RfDirection::rx, 0, 30.0);
    REQUIRE_EQ(backend.rx_gain_indices[11], std::uint8_t{230});
}

TEST_CASE("TX gain converts to and from quarter-dB attenuation")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);

    configuration.setGain(RfDirection::tx, 0, 10.25);
    REQUIRE_EQ(backend.tx_attenuation_indices[22], std::uint16_t{126});
    REQUIRE_EQ(configuration.getGain(RfDirection::tx, 0), 10.25);
    REQUIRE_EQ(configuration.getGainRange(RfDirection::tx).maximum, 41.75);
    REQUIRE_EQ(configuration.getGainRange(RfDirection::tx).step, 0.25);
}

TEST_CASE("RF configuration rejects invalid numeric requests before backend calls")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);

    REQUIRE_THROWS_AS(
        configuration.setFrequency(RfDirection::rx, 0, -1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(
        configuration.setFrequency(
            RfDirection::rx, 0,
            std::ldexp(1.0, std::numeric_limits<std::uint64_t>::digits)),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        configuration.setSampleRate(
            RfDirection::rx, 0, std::numeric_limits<double>::infinity()),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        configuration.setBandwidth(
            RfDirection::rx, 0, std::numeric_limits<double>::quiet_NaN()),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        configuration.setGain(RfDirection::rx, 0, 30.5), std::invalid_argument);
    REQUIRE_THROWS_AS(
        configuration.setGain(RfDirection::tx, 0, 42.0), std::invalid_argument);
}

TEST_CASE("RF configuration rejects bad construction and channel indices")
{
    FakeRfBackend backend;
    REQUIRE_THROWS_AS(
        RfConfiguration(
            backend,
            ChannelMap<std::uint32_t, std::uint32_t>({11}, {22}),
            GainProfile::x_series,
            {},
            {{20, 20.0, 10, 10}},
            {167}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        RfConfiguration(
            backend,
            ChannelMap<std::uint32_t, std::uint32_t>({11}, {}),
            GainProfile::x_series,
            {{10, std::numeric_limits<double>::quiet_NaN(), 10, 10}},
            {},
            {}),
        std::invalid_argument);

    auto configuration = makeConfiguration(backend);
    REQUIRE_THROWS_AS(
        configuration.getFrequency(RfDirection::rx, 1), std::out_of_range);
    REQUIRE_THROWS_AS(
        configuration.setGain(RfDirection::tx, 1, 1.0), std::out_of_range);
}

TEST_CASE("RF backend failures propagate without corrupting cached state")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    const auto before = configuration.cachedRateBandwidth(RfDirection::rx, 0);
    backend.failNextCall();

    REQUIRE_THROWS_AS(
        configuration.setSampleRate(RfDirection::rx, 0, 5'000'000.0),
        std::runtime_error);
    const auto &after = configuration.cachedRateBandwidth(RfDirection::rx, 0);
    REQUIRE_EQ(after.configured_rate, before.configured_rate);
    REQUIRE_EQ(after.actual_bandwidth, before.actual_bandwidth);
}

TEST_CASE("RF configuration rejects impossible backend readbacks")
{
    FakeRfBackend backend;
    auto configuration = makeConfiguration(backend);
    backend.rx_frequency_range = {100, 99};
    REQUIRE_THROWS_AS(
        configuration.getFrequencyRange(RfDirection::rx), std::runtime_error);

    backend.tx_attenuation_indices[22] = 168;
    REQUIRE_THROWS_AS(
        configuration.getGain(RfDirection::tx, 0), std::runtime_error);
}
