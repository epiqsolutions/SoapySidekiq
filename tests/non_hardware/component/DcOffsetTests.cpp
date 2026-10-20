/**
 * @file DcOffsetTests.cpp
 * @brief Exercises production DC calibration selection and rollback with a fake SDK.
 */
#include "TestHarness.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SoapySDR_logf(...) ((void)0)
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
// Match the SDK's counterintuitive enum: automatic is zero, manual is one.
enum skiq_rx_cal_mode_t
{
    skiq_rx_cal_mode_auto = 0,
    skiq_rx_cal_mode_manual = 1,
    unsupported_mode
};
constexpr std::uint32_t skiq_rx_cal_type_dc_offset = 2;
constexpr std::uint32_t other_calibration = 8;
namespace
{
struct Calibration
{
    std::uint32_t available{skiq_rx_cal_type_dc_offset | other_calibration};
    std::uint32_t selected{other_calibration};
    skiq_rx_cal_mode_t mode{skiq_rx_cal_mode_manual};
};
struct FakeSdk
{
    std::array<Calibration, 10> state;
    std::string events;
    std::vector<std::size_t> failures;
    bool apply_before_error{};
    unsigned handle{};
    std::uint8_t card{};
    int touch(char event, std::uint8_t selected_card, unsigned selected_handle)
    {
        events += event;
        card = selected_card;
        handle = selected_handle;
        for (const auto failure : failures)
            if (events.size() == failure)
                return -EIO;
        return 0;
    }
} sdk;
} // namespace
int skiq_read_rx_cal_types_avail(std::uint8_t card, unsigned handle, std::uint32_t *mask)
{
    const int status = sdk.touch('A', card, handle);
    if (status == 0)
        *mask = sdk.state.at(handle).available;
    return status;
}
int skiq_read_rx_cal_type_mask(std::uint8_t card, unsigned handle, std::uint32_t *mask)
{
    const int status = sdk.touch('R', card, handle);
    if (status == 0)
        *mask = sdk.state.at(handle).selected;
    return status;
}
int skiq_read_rx_cal_mode(std::uint8_t card, unsigned handle, skiq_rx_cal_mode_t *mode)
{
    const int status = sdk.touch('M', card, handle);
    if (status == 0)
        *mode = sdk.state.at(handle).mode;
    return status;
}
int skiq_write_rx_cal_type_mask(std::uint8_t card, unsigned handle, std::uint32_t mask)
{
    const int status = sdk.touch('T', card, handle);
    if (status == 0 || sdk.apply_before_error)
        sdk.state.at(handle).selected = mask;
    return status;
}
int skiq_write_rx_cal_mode(std::uint8_t card, unsigned handle, skiq_rx_cal_mode_t mode)
{
    const int status = sdk.touch('W', card, handle);
    if (status == 0 || sdk.apply_before_error)
        sdk.state.at(handle).mode = mode;
    return status;
}
/** Minimal SDK/card state; calibration and validation methods are production code. */
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
    std::uint8_t card{5}, num_rx_channels{2}, num_tx_channels{2};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    bool hasDCOffsetMode(int, std::size_t) const;
    void setDCOffsetMode(int, std::size_t, bool);
    bool getDCOffsetMode(int, std::size_t) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "DcOffsetMethods.inc"

TEST_CASE("DC capability uses the available mask and selected mapped handle")
{
    SoapySidekiq device;
    REQUIRE(device.hasDCOffsetMode(SOAPY_SDR_RX, 1));
    REQUIRE_EQ(sdk.handle, 7u);
    REQUIRE_EQ(sdk.card, device.card);
    sdk.state[7].available = other_calibration;
    REQUIRE(!device.hasDCOffsetMode(SOAPY_SDR_RX, 1));
    REQUIRE(!device.hasDCOffsetMode(SOAPY_SDR_TX, 0));
}
TEST_CASE("DC readback requires both the selected DC bit and automatic mode zero")
{
    SoapySidekiq device;
    auto &state = sdk.state[7];
    state.mode = skiq_rx_cal_mode_auto;
    REQUIRE(!device.getDCOffsetMode(SOAPY_SDR_RX, 1));
    state.selected |= skiq_rx_cal_type_dc_offset;
    REQUIRE(device.getDCOffsetMode(SOAPY_SDR_RX, 1));
    state.mode = skiq_rx_cal_mode_manual;
    REQUIRE(!device.getDCOffsetMode(SOAPY_SDR_RX, 1));
}
TEST_CASE("enabling DC preserves other calibration types and selects automatic mode")
{
    SoapySidekiq device;
    device.setDCOffsetMode(SOAPY_SDR_RX, 1, true);
    REQUIRE_EQ(sdk.events, std::string{"ARMTW"});
    REQUIRE_EQ(sdk.handle, 7u);
    REQUIRE_EQ(sdk.state[7].selected, other_calibration | skiq_rx_cal_type_dc_offset);
    REQUIRE(sdk.state[7].mode == skiq_rx_cal_mode_auto);
    REQUIRE_EQ(sdk.state[3].selected, other_calibration);
    REQUIRE(device.getDCOffsetMode(SOAPY_SDR_RX, 1));
}
TEST_CASE("disabling DC clears only its bit without changing the shared automatic mode")
{
    SoapySidekiq device;
    sdk.state[3].selected |= skiq_rx_cal_type_dc_offset;
    sdk.state[3].mode = skiq_rx_cal_mode_auto;
    device.setDCOffsetMode(SOAPY_SDR_RX, 0, false);
    REQUIRE_EQ(sdk.events, std::string{"ART"});
    REQUIRE_EQ(sdk.state[3].selected, other_calibration);
    REQUIRE(sdk.state[3].mode == skiq_rx_cal_mode_auto);
}
TEST_CASE("already selected automatic DC performs no configuration writes")
{
    SoapySidekiq device;
    sdk.state[3].selected |= skiq_rx_cal_type_dc_offset;
    sdk.state[3].mode = skiq_rx_cal_mode_auto;
    device.setDCOffsetMode(SOAPY_SDR_RX, 0, true);
    REQUIRE_EQ(sdk.events, std::string{"ARM"});
}
TEST_CASE("unsupported DC requests are rejected without mutating calibration state")
{
    SoapySidekiq device;
    sdk.state[3].available = other_calibration;
    REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_RX, 0, true), std::invalid_argument);
    device.setDCOffsetMode(SOAPY_SDR_RX, 0, false);
    REQUIRE(!device.getDCOffsetMode(SOAPY_SDR_RX, 0));
    REQUIRE_EQ(sdk.events, std::string{"AAA"});
    sdk.events.clear();
    REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_TX, 0, true), std::invalid_argument);
    device.setDCOffsetMode(SOAPY_SDR_TX, 0, false);
    REQUIRE(!device.getDCOffsetMode(SOAPY_SDR_TX, 0));
    REQUIRE(sdk.events.empty());
}
TEST_CASE("DC APIs reject invalid direction and channel before SDK calls")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.hasDCOffsetMode(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.getDCOffsetMode(9, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(device.setDCOffsetMode(9, 0, false), std::invalid_argument);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const std::size_t channel : {std::size_t{2}, std::numeric_limits<std::size_t>::max()})
        {
            REQUIRE_THROWS_AS(device.hasDCOffsetMode(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.getDCOffsetMode(direction, channel), std::out_of_range);
            REQUIRE_THROWS_AS(device.setDCOffsetMode(direction, channel, false), std::out_of_range);
        }
    REQUIRE(sdk.events.empty());
}
TEST_CASE("failed DC snapshot reads stop before configuration writes")
{
    for (const std::size_t failure : {1u, 2u, 3u})
    {
        SoapySidekiq device;
        sdk.failures = {failure};
        REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_RX, 0, true), std::runtime_error);
        REQUIRE_EQ(sdk.events.size(), failure);
        REQUIRE_EQ(sdk.state[3].selected, other_calibration);
        REQUIRE(sdk.state[3].mode == skiq_rx_cal_mode_manual);
    }
}
TEST_CASE("failed DC mask write restores its snapshot before returning an error")
{
    SoapySidekiq device;
    sdk.failures = {4};
    sdk.apply_before_error = true;
    REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_RX, 0, true), std::runtime_error);
    REQUIRE_EQ(sdk.events, std::string{"ARMTT"});
    REQUIRE_EQ(sdk.state[3].selected, other_calibration);
    REQUIRE(sdk.state[3].mode == skiq_rx_cal_mode_manual);
}
TEST_CASE("failed DC automatic-mode write restores the prior mode and selected mask")
{
    SoapySidekiq device;
    sdk.failures = {5};
    sdk.apply_before_error = true;
    REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_RX, 0, true), std::runtime_error);
    REQUIRE_EQ(sdk.events, std::string{"ARMTWWT"});
    REQUIRE_EQ(sdk.state[3].selected, other_calibration);
    REQUIRE(sdk.state[3].mode == skiq_rx_cal_mode_manual);
}
TEST_CASE("failed DC compensation is included in the reported error")
{
    SoapySidekiq device;
    sdk.failures = {5, 7};
    bool threw = false;
    try
    {
        device.setDCOffsetMode(SOAPY_SDR_RX, 0, true);
    }
    catch (const std::runtime_error &error)
    {
        threw = true;
        const std::string text(error.what());
        REQUIRE(text.find("skiq_write_rx_cal_mode") != std::string::npos);
        REQUIRE(text.find("mask status " + std::to_string(-EIO)) != std::string::npos);
    }
    REQUIRE(threw);
    REQUIRE_EQ(sdk.state[3].selected, other_calibration | skiq_rx_cal_type_dc_offset);
}
TEST_CASE("DC getter propagates SDK failures and rejects unsupported calibration modes")
{
    for (const std::size_t failure : {1u, 2u, 3u})
    {
        SoapySidekiq device;
        sdk.state[3].selected |= skiq_rx_cal_type_dc_offset;
        sdk.failures = {failure};
        REQUIRE_THROWS_AS(device.getDCOffsetMode(SOAPY_SDR_RX, 0), std::runtime_error);
    }
    SoapySidekiq device;
    sdk.state[3].selected |= skiq_rx_cal_type_dc_offset;
    sdk.state[3].mode = unsupported_mode;
    REQUIRE_THROWS_AS(device.getDCOffsetMode(SOAPY_SDR_RX, 0), std::runtime_error);
    REQUIRE_THROWS_AS(device.setDCOffsetMode(SOAPY_SDR_RX, 0, true), std::runtime_error);
}
