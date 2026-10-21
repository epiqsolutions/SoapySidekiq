/**
 * @file RateBandwidthTests.cpp
 * @brief Checks production RF configuration validation and fresh hardware readback.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/NumericConversion.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <limits>
#include <functional>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;

namespace
{
struct Configuration
{
    std::uint32_t rate{20000000}, bandwidth{18000000}, actual_bandwidth{17999999};
    double actual_rate{20000000.25};
};
struct FakeSdk
{
    std::array<Configuration, 10> state;
    unsigned reads{}, writes{}, handle{};
    std::vector<unsigned> read_handles;
    std::vector<int> read_directions;
    int direction{}, write_status{};
    unsigned fail_read{}, invalid_read{}, omit_bandwidth_at{};
    bool coupled{}, omit_actual{}, write_before_error{};
    std::uint8_t card{};
    std::uint32_t written_rate{}, written_bw{};
} sdk;
int writeConfiguration(int direction, std::uint8_t card, unsigned handle, std::uint32_t rate,
                       std::uint32_t bandwidth)
{
    ++sdk.writes;
    sdk.direction = direction;
    sdk.card = card;
    sdk.handle = handle;
    sdk.written_rate = rate;
    sdk.written_bw = bandwidth;
    if (sdk.write_status != 0 && !sdk.write_before_error)
        return sdk.write_status;
    auto &current = sdk.state.at(handle);
    current.rate = rate;
    current.actual_rate = rate + 0.25;
    current.bandwidth = bandwidth;
    current.actual_bandwidth = bandwidth == 0 ? 12345 : bandwidth - 1;
    if (sdk.coupled)
        for (auto &state : sdk.state)
        {
            state.rate = rate;
            state.actual_rate = rate + 0.25;
        }
    return sdk.write_status;
}
int readConfiguration(int direction, std::uint8_t card, unsigned handle, std::uint32_t *rate,
                      double *actual_rate, std::uint32_t *bandwidth, std::uint32_t *actual_bw)
{
    ++sdk.reads;
    sdk.read_handles.push_back(handle);
    sdk.read_directions.push_back(direction);
    sdk.direction = direction;
    sdk.card = card;
    sdk.handle = handle;
    const auto &current = sdk.state.at(handle);
    *rate = current.rate;
    *bandwidth = current.bandwidth;
    const bool omit_bandwidth = sdk.reads == sdk.omit_bandwidth_at;
    // Model partial output on error, or omitted actual bandwidth on success.
    if (!omit_bandwidth)
        *actual_bw = 777;
    if (sdk.reads == sdk.fail_read)
        return -EIO;
    if (!omit_bandwidth)
        *actual_bw = current.actual_bandwidth;
    if (!sdk.omit_actual)
        *actual_rate = sdk.reads == sdk.invalid_read ? std::numeric_limits<double>::quiet_NaN()
                                                     : current.actual_rate;
    return 0;
}
} // namespace
int skiq_write_rx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t rate,
                                            std::uint32_t bandwidth)
{
    return writeConfiguration(SOAPY_SDR_RX, card, handle, rate, bandwidth);
}
int skiq_write_tx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t rate,
                                            std::uint32_t bandwidth)
{
    return writeConfiguration(SOAPY_SDR_TX, card, handle, rate, bandwidth);
}
int skiq_read_rx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t *rate,
                                           double *actual_rate, std::uint32_t *bandwidth,
                                           std::uint32_t *actual_bw)
{
    return readConfiguration(SOAPY_SDR_RX, card, handle, rate, actual_rate, bandwidth, actual_bw);
}
int skiq_read_tx_sample_rate_and_bandwidth(std::uint8_t card, unsigned handle, std::uint32_t *rate,
                                           double *actual_rate, std::uint32_t *bandwidth,
                                           std::uint32_t *actual_bw)
{
    return readConfiguration(SOAPY_SDR_TX, card, handle, rate, actual_rate, bandwidth, actual_bw);
}

/** Minimal state for the production methods; fake SDK output stays separate. */
class SoapySidekiq
{
  public:
    struct Entry
    {
        unsigned handle;
    };
    struct Parameters
    {
        std::array<Entry, 2> rx_param{{{3}, {7}}};
        std::array<Entry, 2> tx_param{{{6}, {2}}};
    } param;
    struct RateAndBandwidth
    {
        std::uint32_t configured_rate;
        double actual_rate;
        std::uint32_t actual_bandwidth;
    };
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{2};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    bool configuration_blocked{};
    unsigned configuration_attempts{};
    // Script the device/session boundary; the real ownership gate is covered
    // by RxLifecycleTests and RxStreamSessionTests.
    void configureWhileRxInactive(const std::function<void()> &configure)
    {
        ++configuration_attempts;
        if (configuration_blocked)
            throw std::logic_error("RX owns hardware");
        configure();
    }
    void setSampleRate(int, std::size_t, double);
    double getSampleRate(int, std::size_t) const;
    void setBandwidth(int, std::size_t, double);
    double getBandwidth(int, std::size_t) const;
    void validateInitialRateConfiguration();
    RateAndBandwidth readRateAndBandwidth(int, std::size_t) const;
    void applyRateAndBandwidth(int, std::size_t, std::uint32_t, std::uint32_t);
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "RateBandwidthMethods.inc"

TEST_CASE("sample rate changes clamp current hardware bandwidth and validate actual readback")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        device.setSampleRate(direction, 1, 10000000.75);
        REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? 7u : 2u);
        REQUIRE_EQ(sdk.card, device.card);
        REQUIRE_EQ(sdk.direction, direction);
        REQUIRE_EQ(sdk.written_rate, std::uint32_t{10000000});
        REQUIRE_EQ(sdk.written_bw, std::uint32_t{10000000});
        REQUIRE_EQ(sdk.reads, 2u); // Prefetch plus validated post-write readback.
        REQUIRE_EQ(device.getSampleRate(direction, 1), 10000000.25);
        REQUIRE_EQ(device.getBandwidth(direction, 1), 9999999.0);
        REQUIRE_EQ(device.getSampleRate(direction, 0), 20000000.25);
        REQUIRE_EQ(device.getBandwidth(direction, 0), 17999999.0);
    }
}
TEST_CASE("raising sample rate preserves fresh actual hardware bandwidth")
{
    SoapySidekiq device;
    sdk.state[3].actual_bandwidth = 12000000;
    device.setSampleRate(SOAPY_SDR_RX, 0, 30000000);
    REQUIRE_EQ(sdk.written_bw, std::uint32_t{12000000});
}
TEST_CASE("bandwidth changes preserve a coupled hardware rate changed by the other direction")
{
    SoapySidekiq device;
    sdk.coupled = true;
    device.setSampleRate(SOAPY_SDR_TX, 0, 7000000);
    REQUIRE_EQ(device.getSampleRate(SOAPY_SDR_RX, 0), 7000000.25);
    device.setBandwidth(SOAPY_SDR_RX, 0, 1000000);
    REQUIRE_EQ(sdk.written_rate, std::uint32_t{7000000});
    REQUIRE_EQ(device.getSampleRate(SOAPY_SDR_RX, 0), 7000000.25);
    REQUIRE_EQ(device.getBandwidth(SOAPY_SDR_RX, 0), 999999.0);
}
TEST_CASE("RF configuration getters report actual sample rate and bandwidth")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_EQ(device.getSampleRate(direction, 1), 20000000.25);
        REQUIRE_EQ(device.getBandwidth(direction, 1), 17999999.0);
        REQUIRE_EQ(sdk.handle, direction == SOAPY_SDR_RX ? 7u : 2u);
    }
}
TEST_CASE("RF rate and bandwidth reject invalid double conversion before SDK access")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        for (const double value : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), std::ldexp(1.0, 32)})
        {
            REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, value), std::invalid_argument);
            REQUIRE_THROWS_AS(device.setBandwidth(direction, 0, value), std::invalid_argument);
        }
        REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 0), std::invalid_argument);
        REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 0.99), std::invalid_argument);
    }
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("RF configuration validates channels and directions before SDK access")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.setSampleRate(9, 0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setBandwidth(9, 0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getSampleRate(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getBandwidth(9, 0), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        REQUIRE_THROWS_AS(device.setSampleRate(direction, 2, 1), std::out_of_range);
        REQUIRE_THROWS_AS(device.setBandwidth(direction, 2, 1), std::out_of_range);
        REQUIRE_THROWS_AS(device.getSampleRate(direction, 2), std::out_of_range);
        REQUIRE_THROWS_AS(device.getBandwidth(direction, 2), std::out_of_range);
    }
    REQUIRE_EQ(sdk.reads, 0u);
    REQUIRE_EQ(sdk.writes, 0u);
}
TEST_CASE("SDK configuration write failures surface before post-write readback")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
        {
            SoapySidekiq device;
            sdk.write_status = -EIO;
            if (bandwidth)
                REQUIRE_THROWS_AS(device.setBandwidth(direction, 0, 1000000), std::runtime_error);
            else
                REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
            REQUIRE_EQ(sdk.writes, 1u);
            REQUIRE_EQ(sdk.reads, 1u);
            REQUIRE_EQ(device.getSampleRate(direction, 0), 20000000.25);
        }
}
TEST_CASE("prefetch read failures perform no RF configuration write")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
        {
            SoapySidekiq device;
            sdk.fail_read = 1;
            if (bandwidth)
                REQUIRE_THROWS_AS(device.setBandwidth(direction, 0, 1000000), std::runtime_error);
            else
                REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
            REQUIRE_EQ(sdk.writes, 0u);
        }
}
TEST_CASE("partial failing readback surfaces after the hardware configuration changed")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
        {
            SoapySidekiq device;
            sdk.fail_read = 2;
            if (bandwidth)
                REQUIRE_THROWS_AS(device.setBandwidth(direction, 0, 1000000), std::runtime_error);
            else
                REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
            REQUIRE_EQ(sdk.writes, 1u);
            REQUIRE_EQ(sdk.state.at(sdk.handle).rate, bandwidth ? 20000000u : 10000000u);
        }
}
TEST_CASE("invalid post-write readback is rejected without hiding changed hardware")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        sdk.invalid_read = 2;
        REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
        REQUIRE_EQ(sdk.writes, 1u);
        REQUIRE_EQ(sdk.reads, 2u);
        REQUIRE_EQ(device.getSampleRate(direction, 0), 10000000.25);
    }
}
TEST_CASE("RF configuration getters reject invalid and missing actual-rate outputs")
{
    SoapySidekiq device;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        const auto handle = direction == SOAPY_SDR_RX ? 3u : 6u;
        for (const double value : {-1.0, 0.0, 0.5, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), std::ldexp(1.0, 32)})
        {
            sdk.state[handle].actual_rate = value;
            REQUIRE_THROWS_AS(device.getSampleRate(direction, 0), std::runtime_error);
            REQUIRE_THROWS_AS(device.getBandwidth(direction, 0), std::runtime_error);
        }
        sdk.state[handle].actual_rate = 20000000;
        sdk.omit_actual = true;
        REQUIRE_THROWS_AS(device.getSampleRate(direction, 0), std::runtime_error);
        sdk.omit_actual = false;
    }
}
TEST_CASE("zero bandwidth remains an SDK-decided request with actual readback")
{
    SoapySidekiq device;
    device.setBandwidth(SOAPY_SDR_TX, 0, 0);
    REQUIRE_EQ(sdk.written_bw, std::uint32_t{0});
    REQUIRE_EQ(device.getBandwidth(SOAPY_SDR_TX, 0), 12345.0);
}

TEST_CASE("RF getters reject successful SDK calls that omit actual bandwidth")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        sdk.omit_bandwidth_at = 1;
        REQUIRE_THROWS_AS(device.getBandwidth(direction, 0), std::runtime_error);
        sdk.omit_bandwidth_at = 2;
        REQUIRE_THROWS_AS(device.getSampleRate(direction, 0), std::runtime_error);
        REQUIRE_EQ(sdk.writes, 0u);
    }
}

TEST_CASE("missing actual bandwidth after an RF write remains an error")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
        {
            SoapySidekiq device;
            sdk.omit_bandwidth_at = 2;
            if (bandwidth)
                REQUIRE_THROWS_AS(device.setBandwidth(direction, 0, 1000000), std::runtime_error);
            else
                REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
            REQUIRE_EQ(sdk.writes, 1u);
        }
}

TEST_CASE("zero actual filter width is rejected before another RF configuration write")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
    {
        SoapySidekiq device;
        sdk.state[direction == SOAPY_SDR_RX ? 3u : 6u].actual_bandwidth = 0;
        REQUIRE_THROWS_AS(device.getBandwidth(direction, 0), std::runtime_error);
        REQUIRE_THROWS_AS(device.setSampleRate(direction, 0, 10000000), std::runtime_error);
        REQUIRE_EQ(sdk.writes, 0u);
    }
}

TEST_CASE("initial RF validation reads every mapped RX and TX handle without writing hardware")
{
    SoapySidekiq device;
    for (const unsigned handle : {3u, 7u, 6u, 2u})
    {
        sdk.state[handle].actual_rate = 1000000.75 + handle;
        sdk.state[handle].actual_bandwidth = 900000 + handle;
    }
    device.validateInitialRateConfiguration();
    REQUIRE(sdk.read_handles == std::vector<unsigned>({3, 7, 6, 2}));
    REQUIRE(sdk.read_directions ==
            std::vector<int>({SOAPY_SDR_RX, SOAPY_SDR_RX, SOAPY_SDR_TX, SOAPY_SDR_TX}));
    REQUIRE_EQ(sdk.reads, 4u);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            const auto handle = direction == SOAPY_SDR_RX ? device.getRxHandle(channel)
                                                          : device.getTxHandle(channel);
            REQUIRE_EQ(device.getSampleRate(direction, channel), 1000000.75 + handle);
            REQUIRE_EQ(device.getBandwidth(direction, channel), 900000.0 + handle);
        }
    REQUIRE_EQ(sdk.writes, 0u);
}

TEST_CASE("initial RF validation rejects unsafe floating point rates on later handles")
{
    const double invalid_rates[] = {0,
                                    -1,
                                    0.5,
                                    std::ldexp(1.0, 32),
                                    std::numeric_limits<double>::infinity(),
                                    -std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::quiet_NaN()};
    for (const unsigned handle : {7u, 2u})
        for (const auto rate : invalid_rates)
        {
            SoapySidekiq device;
            sdk.state[handle].actual_rate = rate;
            REQUIRE_THROWS_AS(device.validateInitialRateConfiguration(), std::runtime_error);
            REQUIRE_EQ(sdk.reads, handle == 7u ? 2u : 4u);
            REQUIRE_EQ(sdk.handle, handle);
            REQUIRE_EQ(sdk.writes, 0u);
        }
}

TEST_CASE("initial RF validation rejects missing rates and bandwidth outputs")
{
    for (const bool omit_rate : {false, true})
    {
        SoapySidekiq device;
        sdk.omit_actual = omit_rate;
        sdk.omit_bandwidth_at = omit_rate ? 0 : 4;
        REQUIRE_THROWS_AS(device.validateInitialRateConfiguration(), std::runtime_error);
        REQUIRE_EQ(sdk.reads, omit_rate ? 1u : 4u);
        REQUIRE_EQ(sdk.writes, 0u);
    }
}

TEST_CASE("initial RF validation stops at the first failed SDK read")
{
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        SoapySidekiq device;
        sdk.state[3].actual_rate = 1234567;
        sdk.fail_read = failure;
        REQUIRE_THROWS_AS(device.validateInitialRateConfiguration(), std::runtime_error);
        REQUIRE_EQ(sdk.reads, failure);
        REQUIRE_EQ(sdk.writes, 0u);
    }
}

TEST_CASE("initial RF validation skips absent directions and supports empty devices")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX, -1})
    {
        SoapySidekiq device;
        device.num_rx_channels = direction == SOAPY_SDR_RX ? 2 : 0;
        device.num_tx_channels = direction == SOAPY_SDR_TX ? 2 : 0;
        device.validateInitialRateConfiguration();
        REQUIRE_EQ(sdk.reads, direction < 0 ? 0u : 2u);
    }
}

TEST_CASE("RX and TX rate or bandwidth setters reject active RX before SDK access")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
        {
            SoapySidekiq device;
            device.configuration_blocked = true;
            if (bandwidth)
                REQUIRE_THROWS_AS(device.setBandwidth(direction, 1, 1000000), std::logic_error);
            else
                REQUIRE_THROWS_AS(device.setSampleRate(direction, 1, 2000000), std::logic_error);
            REQUIRE_EQ(device.configuration_attempts, 1u);
            REQUIRE_EQ(sdk.reads, 0u);
            REQUIRE_EQ(sdk.writes, 0u);
        }
}

namespace
{
enum class WriteFailure
{
    partial_write,
    readback_error,
    invalid_rate,
    missing_bandwidth
};

void failAfterHardwareChange(const WriteFailure failure)
{
    sdk.coupled = true;
    switch (failure)
    {
    case WriteFailure::partial_write:
        sdk.write_status = -EIO;
        sdk.write_before_error = true;
        break;
    case WriteFailure::readback_error:
        sdk.fail_read = sdk.reads + 2;
        break;
    case WriteFailure::invalid_rate:
        sdk.invalid_read = sdk.reads + 2;
        break;
    case WriteFailure::missing_bandwidth:
        sdk.omit_bandwidth_at = sdk.reads + 2;
        break;
    }
}

} // namespace

TEST_CASE("failed coupled RF transactions remain observable through every fresh getter")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const bool bandwidth : {false, true})
            for (const auto failure : {WriteFailure::partial_write, WriteFailure::readback_error,
                                       WriteFailure::invalid_rate, WriteFailure::missing_bandwidth})
            {
                SoapySidekiq device;
                for (const unsigned handle : {3u, 7u, 6u, 2u})
                    sdk.state[handle].actual_bandwidth = 17000000 + handle;
                device.validateInitialRateConfiguration();
                const auto selected = direction == SOAPY_SDR_RX ? 7u : 2u;
                failAfterHardwareChange(failure);
                if (bandwidth)
                    REQUIRE_THROWS_AS(device.setBandwidth(direction, 1, 700000),
                                      std::runtime_error);
                else
                    REQUIRE_THROWS_AS(device.setSampleRate(direction, 1, 3500000),
                                      std::runtime_error);
                REQUIRE_EQ(sdk.writes, 1u);
                REQUIRE_EQ(sdk.state[selected].rate, bandwidth ? 20000000u : 3500000u);
                REQUIRE_EQ(sdk.state[selected].actual_bandwidth, bandwidth ? 699999u : 3499999u);

                // An SDK error cannot undo a hardware write. Each getter must
                // observe the current mapped channel rather than a prior snapshot.
                sdk.fail_read = sdk.invalid_read = sdk.omit_bandwidth_at = 0;
                for (const int observed_direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
                    for (std::size_t channel = 0; channel < 2; ++channel)
                    {
                        const auto handle = observed_direction == SOAPY_SDR_RX
                                                ? device.getRxHandle(channel)
                                                : device.getTxHandle(channel);
                        REQUIRE_EQ(device.getSampleRate(observed_direction, channel),
                                   sdk.state[handle].actual_rate);
                        REQUIRE_EQ(device.getBandwidth(observed_direction, channel),
                                   static_cast<double>(sdk.state[handle].actual_bandwidth));
                    }
            }
}

TEST_CASE("opposite-direction bandwidth recovery uses the rate left by a failed coupled change")
{
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const auto failure : {WriteFailure::partial_write, WriteFailure::readback_error,
                                   WriteFailure::invalid_rate, WriteFailure::missing_bandwidth})
        {
            SoapySidekiq device;
            device.validateInitialRateConfiguration();
            failAfterHardwareChange(failure);
            REQUIRE_THROWS_AS(device.setSampleRate(direction, 1, 3500000), std::runtime_error);
            const auto other = direction == SOAPY_SDR_RX ? SOAPY_SDR_TX : SOAPY_SDR_RX;
            const auto recovery_handle = other == SOAPY_SDR_RX ? 3u : 6u;
            REQUIRE_EQ(sdk.state[recovery_handle].rate, 3500000u);
            sdk.write_status = 0;
            sdk.write_before_error = false;
            sdk.fail_read = sdk.invalid_read = sdk.omit_bandwidth_at = 0;
            device.setBandwidth(other, 0, 600000);
            REQUIRE_EQ(sdk.writes, 2u);
            REQUIRE_EQ(sdk.handle, recovery_handle);
            REQUIRE_EQ(sdk.direction, other);
            REQUIRE_EQ(sdk.written_rate, 3500000u);
            REQUIRE_EQ(sdk.written_bw, 600000u);
            REQUIRE_EQ(device.getBandwidth(other, 0), 599999.0);
            REQUIRE_EQ(device.getSampleRate(direction, 1), 3500000.25);
        }
}
