/**
 * @file SettingsTests.cpp
 * @brief Exercises production settings validation and failed-write recovery.
 */
#include "TestHarness.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#define SoapySDR_logf(...) ((void)0)
#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
using skiq_rx_hdl_t = unsigned;
using skiq_tx_hdl_t = unsigned;
enum skiq_iq_order_t
{
    skiq_iq_order_iq,
    skiq_iq_order_qi
};
enum skiq_data_src_t
{
    skiq_data_src_iq,
    skiq_data_src_counter
};
namespace SoapySDR
{
struct ArgInfo
{
    enum Type
    {
        BOOL,
        STRING
    } type{STRING};
    std::string key, value, name, description;
    std::vector<std::string> options;
};
using ArgInfoList = std::vector<ArgInfo>;
} // namespace SoapySDR
namespace
{
struct FakeSdk
{
    std::array<skiq_data_src_t, 8> sources{};
    std::vector<unsigned> reads, writes;
    skiq_iq_order_t order{skiq_iq_order_qi};
    unsigned iq_calls{};
    int iq_status{}, read_fail_handle{-1}, write_fail_call{-1}, rollback_fail_call{-1};
    int scalar_status{};
} sdk;
} // namespace
int skiq_write_iq_order_mode(std::uint8_t, skiq_iq_order_t order)
{
    ++sdk.iq_calls;
    if (!sdk.iq_status)
        sdk.order = order;
    return sdk.iq_status;
}
int skiq_read_rx_data_src(std::uint8_t, unsigned handle, skiq_data_src_t *source)
{
    sdk.reads.push_back(handle);
    if (static_cast<int>(handle) == sdk.read_fail_handle)
        return -11;
    *source = sdk.sources.at(handle);
    return 0;
}
int skiq_write_rx_data_src(std::uint8_t, unsigned handle, skiq_data_src_t source)
{
    sdk.writes.push_back(handle);
    // Simulate a write taking effect even when the SDK reports failure.
    sdk.sources.at(handle) = source;
    const int call = static_cast<int>(sdk.writes.size());
    if (call == sdk.write_fail_call)
        return -12;
    if (call == sdk.rollback_fail_call)
        return -13;
    return 0;
}
int skiq_read_rx_overload_state(std::uint8_t, unsigned, std::uint8_t *value)
{
    *value = 1;
    return sdk.scalar_status;
}
int skiq_read_rx_cal_offset(std::uint8_t, unsigned, double *value)
{
    *value = -2.5;
    return sdk.scalar_status;
}
/** Only SDK state is simulated; settings and channel mapping use production code. */
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
    bool iq_swap{}, counter{}, rfTimeSource{true};
    std::uint64_t sys_freq{40000000};
    double maxValue{2048};
    std::string timetype{"rf_timestamp"};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::size_t getNumChannels(int) const;
    void validateChannel(int, std::size_t) const;
    skiq_rx_hdl_t getRxHandle(std::size_t) const;
    skiq_tx_hdl_t getTxHandle(std::size_t) const;
    skiq_rx_hdl_t getFirstRxHandle() const
    {
        return getRxHandle(0);
    }
    SoapySDR::ArgInfoList getSettingInfo() const;
    void writeSetting(const std::string &, const std::string &);
    std::string readSetting(const std::string &) const;
};
#include "ChannelValidationMethods.inc"
#include "ChannelMappingMethods.inc"
#include "SettingsMethods.inc"

TEST_CASE("settings metadata reports current values and timestamp options")
{
    SoapySidekiq device;
    device.iq_swap = device.counter = true;
    device.timetype = "sys_timestamp";
    const auto metadata = device.getSettingInfo();
    REQUIRE_EQ(metadata.size(), std::size_t{5});
    for (const auto &entry : metadata)
    {
        REQUIRE_EQ(entry.value, device.readSetting(entry.key));
        if (entry.key == "timetype")
            REQUIRE_EQ(entry.options.size(), std::size_t{2});
        else
            REQUIRE(entry.options.empty());
    }
    REQUIRE_EQ(metadata[0].type, SoapySDR::ArgInfo::BOOL);
    REQUIRE_EQ(metadata[1].type, SoapySDR::ArgInfo::BOOL);
}
TEST_CASE("boolean settings accept documented case insensitive forms and keys")
{
    for (const auto &key : {"IQ_SWAP", "CoUnTeR"})
        for (const auto &value : {"true", "TRUE", "TrUe", "1", "false", "FALSE", "0"})
        {
            SoapySidekiq device;
            device.writeSetting(key, value);
            const bool enabled = std::string(value) == "1" || std::tolower(value[0]) == 't';
            REQUIRE_EQ(device.readSetting(key), std::string(enabled ? "true" : "false"));
            if (std::string(key) == "IQ_SWAP")
                REQUIRE_EQ(sdk.order, enabled ? skiq_iq_order_iq : skiq_iq_order_qi);
            else
            {
                REQUIRE_EQ(sdk.sources[3], enabled ? skiq_data_src_counter : skiq_data_src_iq);
                REQUIRE_EQ(sdk.sources[7], sdk.sources[3]);
            }
        }
}
TEST_CASE("invalid boolean requests leave cache and hardware untouched")
{
    SoapySidekiq device;
    for (const auto &key : {"iq_swap", "counter"})
        for (const auto &value : {"", "yes", "2", " true", "false ", "-1"})
            REQUIRE_THROWS_AS(device.writeSetting(key, value), std::invalid_argument);
    REQUIRE_EQ(sdk.iq_calls, 0u);
    REQUIRE(sdk.reads.empty());
    REQUIRE(sdk.writes.empty());
    REQUIRE(!device.iq_swap && !device.counter);
}
TEST_CASE("failed IQ order writes preserve the last successful cached value")
{
    SoapySidekiq device;
    device.writeSetting("iq_swap", "true");
    sdk.iq_status = -5;
    REQUIRE_THROWS_AS(device.writeSetting("iq_swap", "false"), std::runtime_error);
    REQUIRE_EQ(device.readSetting("iq_swap"), std::string{"true"});
}
TEST_CASE("counter writes snapshot every mapped handle before changing sources")
{
    SoapySidekiq device;
    device.writeSetting("counter", "true");
    REQUIRE((sdk.reads == std::vector<unsigned>{3, 7}));
    REQUIRE((sdk.writes == std::vector<unsigned>{3, 7}));
    REQUIRE(device.counter);
}
TEST_CASE("counter snapshot failures prevent every write")
{
    SoapySidekiq device;
    sdk.read_fail_handle = 7;
    REQUIRE_THROWS_AS(device.writeSetting("counter", "true"), std::runtime_error);
    REQUIRE(sdk.writes.empty());
    REQUIRE(!device.counter);
}
TEST_CASE("counter failure restores mixed source snapshots including failed writes")
{
    SoapySidekiq device;
    sdk.sources[3] = skiq_data_src_counter;
    sdk.write_fail_call = 2;
    REQUIRE_THROWS_AS(device.writeSetting("counter", "true"), std::runtime_error);
    REQUIRE((sdk.writes == std::vector<unsigned>{3, 7, 7, 3}));
    REQUIRE_EQ(sdk.sources[3], skiq_data_src_counter);
    REQUIRE_EQ(sdk.sources[7], skiq_data_src_iq);
    REQUIRE(!device.counter);
}
TEST_CASE("failed counter disable preserves the last successful cached mode")
{
    SoapySidekiq device;
    device.writeSetting("counter", "true");
    sdk.writes.clear();
    sdk.write_fail_call = 2;
    REQUIRE_THROWS_AS(device.writeSetting("counter", "false"), std::runtime_error);
    REQUIRE_EQ(device.readSetting("counter"), std::string{"true"});
    REQUIRE_EQ(sdk.sources[3], skiq_data_src_counter);
    REQUIRE_EQ(sdk.sources[7], skiq_data_src_counter);
}
TEST_CASE("counter first write failure is also compensated")
{
    SoapySidekiq device;
    sdk.write_fail_call = 1;
    REQUIRE_THROWS_AS(device.writeSetting("counter", "true"), std::runtime_error);
    REQUIRE((sdk.writes == std::vector<unsigned>{3, 3}));
    REQUIRE_EQ(sdk.sources[3], skiq_data_src_iq);
}
TEST_CASE("counter rollback failures are reported after all restores are attempted")
{
    SoapySidekiq device;
    sdk.write_fail_call = 2;
    sdk.rollback_fail_call = 3;
    std::string message;
    try
    {
        device.writeSetting("counter", "true");
    }
    catch (const std::runtime_error &error)
    {
        message = error.what();
    }
    REQUIRE(message.find("rollback failed: -13") != std::string::npos);
    REQUIRE_EQ(sdk.writes.size(), std::size_t{4});
    REQUIRE(!device.counter);
}
TEST_CASE("counter mode requires RX channels")
{
    SoapySidekiq device;
    device.num_rx_channels = 0;
    REQUIRE_THROWS_AS(device.writeSetting("counter", "true"), std::runtime_error);
    REQUIRE(sdk.writes.empty());
    REQUIRE(!device.counter);
}
TEST_CASE("timestamp setting validates before changing either cached field")
{
    SoapySidekiq device;
    device.writeSetting("TIMETYPE", "sys_timestamp");
    REQUIRE(!device.rfTimeSource);
    REQUIRE_THROWS_AS(device.writeSetting("timetype", "bad"), std::invalid_argument);
    REQUIRE_EQ(device.timetype, std::string{"sys_timestamp"});
    REQUIRE(!device.rfTimeSource);
    device.writeSetting("timetype", "rf_timestamp");
    REQUIRE(device.rfTimeSource);
}
TEST_CASE("unknown and read only setting writes are rejected")
{
    SoapySidekiq device;
    REQUIRE_THROWS_AS(device.readSetting("bad"), std::invalid_argument);
    for (const auto &key : {"bad", "FULL_SCALE", "sys_clock_freq", "overload", "cal_offset"})
        REQUIRE_THROWS_AS(device.writeSetting(key, "1"), std::invalid_argument);
    REQUIRE(sdk.writes.empty());
    REQUIRE_EQ(device.readSetting("overload"), std::string{"1"});
    REQUIRE_EQ(device.readSetting("cal_offset"), std::to_string(-2.5));
    sdk.scalar_status = -5;
    REQUIRE_THROWS_AS(device.readSetting("overload"), std::runtime_error);
    REQUIRE_THROWS_AS(device.readSetting("cal_offset"), std::runtime_error);
}
