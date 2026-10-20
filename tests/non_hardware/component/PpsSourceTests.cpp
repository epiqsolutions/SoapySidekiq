/**
 * @file PpsSourceTests.cpp
 * @brief Exercises production PPS selection, kernel fallback, and edge detection.
 */
#include "TestHarness.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#define SoapySDR_logf(...) ((void)0)
enum skiq_1pps_source_t
{
    skiq_1pps_source_unavailable = -1,
    skiq_1pps_source_external,
    skiq_1pps_source_host,
    invalid_source
};
namespace
{
constexpr char SOURCE_1PPS_UNAVAILABLE[] = "1pps_source_unavailable";
constexpr char SOURCE_1PPS_EXTERNAL[] = "1pps_source_external";
constexpr char SOURCE_1PPS_INTERNAL[] = "1pps_source_internal";
std::string KERNEL_1PPS_SOURCE_PATH;
struct FakeSdk
{
    skiq_1pps_source_t source{skiq_1pps_source_unavailable};
    int read_status{}, write_status{}, timestamp_error_call{-1}, wait_status{};
    unsigned reads{}, writes{}, timestamp_reads{}, waits{};
    std::vector<std::uint64_t> timestamps{10, 11};
    std::uint8_t card{};
} sdk;
/** Redirect only the kernel endpoint; the parser and file IO remain production. */
class KernelFile
{
  public:
    std::string path;
    KernelFile()
    {
        char name[] = "/tmp/soapysidekiq-pps-XXXXXX";
        const int fd = mkstemp(name);
        if (fd < 0)
            throw std::runtime_error("Cannot create PPS fixture");
        close(fd);
        path = name;
        KERNEL_1PPS_SOURCE_PATH = path;
        sdk = {};
    }
    ~KernelFile()
    {
        std::remove(path.c_str());
    }
    void contents(const std::string &text)
    {
        std::ofstream(path) << text;
    }
    std::string contents() const
    {
        std::ifstream input(path);
        std::ostringstream output;
        output << input.rdbuf();
        return output.str();
    }
};
} // namespace
int skiq_read_1pps_source(std::uint8_t card, skiq_1pps_source_t *source)
{
    ++sdk.reads;
    sdk.card = card;
    *source = sdk.source;
    return sdk.read_status;
}
int skiq_write_1pps_source(std::uint8_t card, skiq_1pps_source_t source)
{
    ++sdk.writes;
    sdk.card = card;
    if (!sdk.write_status)
        sdk.source = source;
    return sdk.write_status;
}
int skiq_read_last_1pps_timestamp(std::uint8_t card, std::uint64_t *rf, std::uint64_t *sys)
{
    const auto call = sdk.timestamp_reads++;
    sdk.card = card;
    if (static_cast<int>(call) == sdk.timestamp_error_call)
        return -5;
    *rf = 0;
    *sys = sdk.timestamps.at(call);
    return 0;
}
int testSleep(unsigned duration)
{
    REQUIRE_EQ(duration, 1000000u);
    ++sdk.waits;
    return sdk.wait_status;
}
/** SDK state and elapsed time are injected; API and kernel helpers are real. */
class SoapySidekiq
{
  public:
    std::uint8_t card{5};
    std::vector<std::string> listTimeSources() const;
    std::string getTimeSource() const;
    void setTimeSource(const std::string &);
};
#define usleep testSleep
#include "PpsMethods.inc"
#undef usleep

TEST_CASE("PPS metadata enumerates selectable SDK sources")
{
    KernelFile file;
    SoapySidekiq device;
    REQUIRE_EQ(device.listTimeSources().size(), std::size_t{3});
    for (const auto &source : device.listTimeSources())
    {
        sdk.timestamp_reads = 0;
        device.setTimeSource(source);
        REQUIRE_EQ(device.getTimeSource(), source);
        REQUIRE_EQ(sdk.card, device.card);
    }
}
TEST_CASE("invalid PPS source does not write SDK or kernel configuration")
{
    KernelFile file;
    file.contents("unchanged");
    SoapySidekiq device;
    for (const auto &source : {"", "external", "bad", "1pps_source_external "})
        REQUIRE_THROWS_AS(device.setTimeSource(source), std::invalid_argument);
    REQUIRE_EQ(sdk.writes, 0u);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
    REQUIRE_EQ(file.contents(), std::string{"unchanged"});
}
TEST_CASE("PPS source names accept case insensitive selection")
{
    KernelFile file;
    SoapySidekiq device;
    device.setTimeSource("1PPS_SOURCE_EXTERNAL");
    REQUIRE_EQ(sdk.source, skiq_1pps_source_external);
}
TEST_CASE("kernel PPS parser selects bracketed tokens including unavailable")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.read_status = -95;
    for (const auto &source :
         {std::string{"external"}, std::string{"internal-gps"}, std::string{"unavailable"}})
    {
        file.contents("other [" + source + "] external\n");
        const std::string expected = source == "external"       ? SOURCE_1PPS_EXTERNAL
                                     : source == "internal-gps" ? SOURCE_1PPS_INTERNAL
                                                                : SOURCE_1PPS_UNAVAILABLE;
        REQUIRE_EQ(device.getTimeSource(), expected);
    }
}
TEST_CASE("malformed or missing kernel PPS source reports failure")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.read_status = -95;
    for (const auto &text : {"", "external internal-gps", "[unknown]", "[external", "[]"})
    {
        file.contents(text);
        REQUIRE_THROWS_AS(device.getTimeSource(), std::runtime_error);
    }
    KERNEL_1PPS_SOURCE_PATH = file.path + "/absent";
    REQUIRE_THROWS_AS(device.getTimeSource(), std::runtime_error);
}
TEST_CASE("SDK PPS read errors include status when fallback is unavailable")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.read_status = -7;
    std::string message;
    try
    {
        device.getTimeSource();
    }
    catch (const std::runtime_error &error)
    {
        message = error.what();
    }
    REQUIRE(message.find("skiq_read_1pps_source") != std::string::npos);
    REQUIRE(message.find("-7") != std::string::npos);
}
TEST_CASE("unknown SDK PPS enum is an error")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.source = invalid_source;
    REQUIRE_THROWS_AS(device.getTimeSource(), std::runtime_error);
}
TEST_CASE("kernel write fallback maps external and internal names")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.write_status = -95;
    device.setTimeSource(SOURCE_1PPS_EXTERNAL);
    REQUIRE_EQ(file.contents(), std::string{"external"});
    sdk.timestamp_reads = 0;
    device.setTimeSource(SOURCE_1PPS_INTERNAL);
    REQUIRE_EQ(file.contents(), std::string{"internal-gps"});
}
TEST_CASE("unavailable kernel write is rejected without overwriting the endpoint")
{
    KernelFile file;
    file.contents("unchanged");
    SoapySidekiq device;
    sdk.write_status = -95;
    REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_UNAVAILABLE), std::runtime_error);
    REQUIRE_EQ(file.contents(), std::string{"unchanged"});
    REQUIRE_EQ(sdk.waits, 0u);
}
TEST_CASE("failed PPS configuration does not start pulse observation")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.write_status = -8;
    KERNEL_1PPS_SOURCE_PATH = file.path + "/absent";
    REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_EXTERNAL), std::runtime_error);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
    REQUIRE_EQ(sdk.waits, 0u);
}
TEST_CASE("kernel buffered write failures are reported before pulse observation")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.write_status = -95;
    KERNEL_1PPS_SOURCE_PATH = "/dev/full";
    REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_EXTERNAL), std::runtime_error);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
}
TEST_CASE("PPS observation accepts a transition from a zero timestamp")
{
    KernelFile file;
    SoapySidekiq device;
    sdk.timestamps = {0, 1};
    device.setTimeSource(SOURCE_1PPS_EXTERNAL);
    REQUIRE_EQ(sdk.timestamp_reads, 2u);
    REQUIRE_EQ(sdk.waits, 1u);
}
TEST_CASE("unchanged PPS timestamps report no edge with bounded observations")
{
    for (const auto timestamp : {std::uint64_t{0}, std::uint64_t{10}})
    {
        KernelFile file;
        SoapySidekiq device;
        sdk.timestamps = {timestamp, timestamp};
        REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_EXTERNAL), std::runtime_error);
        REQUIRE_EQ(sdk.timestamp_reads, 2u);
        REQUIRE_EQ(sdk.waits, 1u);
    }
}
TEST_CASE("PPS observation reports SDK and interrupted wait failures")
{
    for (int failed_call : {0, 1})
    {
        KernelFile file;
        SoapySidekiq device;
        sdk.timestamp_error_call = failed_call;
        REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_INTERNAL), std::runtime_error);
        REQUIRE_EQ(sdk.timestamp_reads, static_cast<unsigned>(failed_call + 1));
    }
    KernelFile file;
    SoapySidekiq device;
    sdk.wait_status = -1;
    REQUIRE_THROWS_AS(device.setTimeSource(SOURCE_1PPS_EXTERNAL), std::runtime_error);
    REQUIRE_EQ(sdk.timestamp_reads, 1u);
}
TEST_CASE("unavailable PPS selection never waits for a pulse")
{
    KernelFile file;
    SoapySidekiq device;
    device.setTimeSource(SOURCE_1PPS_UNAVAILABLE);
    REQUIRE_EQ(sdk.waits, 0u);
    REQUIRE_EQ(sdk.timestamp_reads, 0u);
}
