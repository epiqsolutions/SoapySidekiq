/**
 * @file ClockTests.cpp
 * @brief Verifies production clock selection and reference-rate validation.
 */
#include "TestHarness.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#define SoapySDR_logf(...) ((void)0)
enum skiq_ref_clock_select_t
{
    skiq_ref_clock_internal,
    skiq_ref_clock_external,
    skiq_ref_clock_host,
    skiq_ref_clock_carrier_edge,
    skiq_ref_clock_invalid
};
namespace
{
struct FakeSdk
{
    skiq_ref_clock_select_t source{skiq_ref_clock_internal};
    std::uint32_t rate{10000000};
    unsigned reads{}, writes{}, rate_reads{};
    std::uint8_t card{};
    int read_status{}, write_status{}, rate_status{};
    bool omit_source{}, omit_rate{};
} sdk;
} // namespace
int skiq_read_ref_clock_select(std::uint8_t card, skiq_ref_clock_select_t *source)
{
    ++sdk.reads;
    sdk.card = card;
    if (!sdk.omit_source)
        *source = sdk.source;
    return sdk.read_status;
}
int skiq_write_ref_clock_select(std::uint8_t card, skiq_ref_clock_select_t source)
{
    ++sdk.writes;
    sdk.card = card;
    if (!sdk.write_status)
        sdk.source = source;
    return sdk.write_status;
}
int skiq_read_ext_ref_clock_freq(std::uint8_t card, std::uint32_t *rate)
{
    ++sdk.rate_reads;
    sdk.card = card;
    if (!sdk.omit_rate)
        *rate = sdk.rate;
    return sdk.rate_status;
}
/** Clock metadata is injected; the API bodies come from Settings.cpp. */
class SoapySidekiq
{
  public:
    struct Parameters
    {
        struct Rf
        {
            std::uint32_t ref_clock_freq{40000000};
        } rf_param;
    } param;
    bool configuration_blocked{};
    unsigned configuration_attempts{};
    // Inject admission here; real retained-ownership serialization is covered
    // by RxLifecycleTests and RxStreamSessionTests.
    void configureWhileRxInactive(const std::function<void()> &configure)
    {
        ++configuration_attempts;
        if (configuration_blocked)
            throw std::logic_error("RX owns hardware");
        configure();
    }
    std::uint8_t card{5};
    SoapySidekiq()
    {
        sdk = {};
    }
    std::vector<std::string> listClockSources() const;
    std::string getClockSource() const;
    void setClockSource(const std::string &);
    double getReferenceClockRate() const;
};
#include "ClockMethods.inc"

TEST_CASE("advertised clock sources can be selected and read back")
{
    SoapySidekiq device;
    const auto sources = device.listClockSources();
    REQUIRE_EQ(sources.size(), std::size_t{2});
    REQUIRE_EQ(sdk.reads, 0u);
    for (const auto &source : sources)
    {
        device.setClockSource(source);
        REQUIRE_EQ(device.getClockSource(), source);
        REQUIRE_EQ(sdk.card, device.card);
    }
    device.setClockSource("EXTERNAL_CLOCK");
    REQUIRE_EQ(sdk.source, skiq_ref_clock_external);
    device.setClockSource("InTeRnAl_ClOcK");
    REQUIRE_EQ(sdk.source, skiq_ref_clock_internal);
}
TEST_CASE("invalid clock sources never silently switch the reference")
{
    SoapySidekiq device;
    sdk.source = skiq_ref_clock_external;
    for (const auto &source : {"", "internal", "external", "internal_clock ", "bad"})
        REQUIRE_THROWS_AS(device.setClockSource(source), std::invalid_argument);
    REQUIRE_EQ(sdk.writes, 0u);
    REQUIRE_EQ(sdk.source, skiq_ref_clock_external);
}
TEST_CASE("clock selection SDK failures include the operation and status")
{
    SoapySidekiq device;
    sdk.write_status = -5;
    std::string message;
    try
    {
        device.setClockSource("external_clock");
    }
    catch (const std::runtime_error &error)
    {
        message = error.what();
    }
    REQUIRE(message.find("skiq_write_ref_clock_select failed: -5") != std::string::npos);
    REQUIRE_EQ(sdk.source, skiq_ref_clock_internal);
    sdk.read_status = -6;
    REQUIRE_THROWS_AS(device.getClockSource(), std::runtime_error);
    REQUIRE_THROWS_AS(device.getReferenceClockRate(), std::runtime_error);
    REQUIRE_EQ(sdk.rate_reads, 0u);
}
TEST_CASE("host reference retains the existing internal clock alias")
{
    SoapySidekiq device;
    sdk.source = skiq_ref_clock_host;
    REQUIRE_EQ(device.getClockSource(), std::string{"internal_clock"});
    REQUIRE_EQ(device.getReferenceClockRate(), 40000000.0);
    REQUIRE_EQ(sdk.rate_reads, 0u);
}
TEST_CASE("unsupported and omitted clock readbacks fail explicitly")
{
    SoapySidekiq device;
    for (const auto source : {skiq_ref_clock_carrier_edge, skiq_ref_clock_invalid})
    {
        sdk.source = source;
        REQUIRE_THROWS_AS(device.getClockSource(), std::runtime_error);
        REQUIRE_THROWS_AS(device.getReferenceClockRate(), std::runtime_error);
    }
    sdk.source = skiq_ref_clock_internal;
    sdk.omit_source = true;
    REQUIRE_THROWS_AS(device.getClockSource(), std::runtime_error);
}
TEST_CASE("reference rate uses selected source and preserves the uint32 range")
{
    SoapySidekiq device;
    device.param.rf_param.ref_clock_freq = 122880000;
    REQUIRE_EQ(device.getReferenceClockRate(), 122880000.0);
    REQUIRE_EQ(sdk.rate_reads, 0u);
    sdk.source = skiq_ref_clock_external;
    REQUIRE_EQ(device.getReferenceClockRate(), 10000000.0);
    REQUIRE_EQ(sdk.card, device.card);
    sdk.rate = std::numeric_limits<std::uint32_t>::max();
    REQUIRE_EQ(device.getReferenceClockRate(), static_cast<double>(sdk.rate));
}
TEST_CASE("zero or omitted reference frequencies are rejected")
{
    SoapySidekiq device;
    device.param.rf_param.ref_clock_freq = 0;
    REQUIRE_THROWS_AS(device.getReferenceClockRate(), std::runtime_error);
    sdk.source = skiq_ref_clock_external;
    sdk.rate = 0;
    REQUIRE_THROWS_AS(device.getReferenceClockRate(), std::runtime_error);
    sdk.rate = 10000000;
    sdk.omit_rate = true;
    REQUIRE_THROWS_AS(device.getReferenceClockRate(), std::runtime_error);
}
TEST_CASE("external clock rate errors report the correct SDK operation")
{
    SoapySidekiq device;
    sdk.source = skiq_ref_clock_external;
    sdk.rate_status = -95;
    std::string message;
    try
    {
        device.getReferenceClockRate();
    }
    catch (const std::runtime_error &error)
    {
        message = error.what();
    }
    REQUIRE(message.find("skiq_read_ext_ref_clock_freq failed: -95") != std::string::npos);
}

TEST_CASE("RX ownership rejects clock changes before touching the reference")
{
    SoapySidekiq device;
    device.configuration_blocked = true;
    for (const auto &source : device.listClockSources())
        REQUIRE_THROWS_AS(device.setClockSource(source), std::logic_error);
    REQUIRE_EQ(sdk.reads + sdk.writes + sdk.rate_reads, 0u);
    REQUIRE_EQ(sdk.source, skiq_ref_clock_internal);
    device.configuration_blocked = false;
    device.setClockSource("external_clock");
    REQUIRE_EQ(sdk.writes, 1u);
    REQUIRE_EQ(sdk.source, skiq_ref_clock_external);
}
