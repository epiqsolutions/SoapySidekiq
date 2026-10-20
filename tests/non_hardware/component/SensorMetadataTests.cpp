/**
 * @file SensorMetadataTests.cpp
 * @brief Verifies production sensor metadata and structured reading descriptions.
 */

#include "TestHarness.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace SoapySDR
{
/** SDK-free subset of the SoapySDR sensor metadata contract. */
struct ArgInfo
{
    enum Type
    {
        BOOL,
        INT,
        FLOAT,
        STRING
    } type{STRING};
    std::string key, value, name, description, units;
};
} // namespace SoapySDR

/** Captures enumeration diagnostics without a SoapySDR dependency. */
void SoapySDR_log(int, const char *) {}
constexpr int SOAPY_SDR_TRACE = 0;

/** Fixture exposing the actual device method bodies without SDK initialization. */
class SoapySidekiq
{
  public:
    std::vector<std::string> listSensors() const;
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const;
};

#include "SensorMetadataMethods.inc"

TEST_CASE("every advertised sensor has named metadata for the same key")
{
    const SoapySidekiq device;
    const auto sensors = device.listSensors();
    REQUIRE_EQ(sensors.size(), std::size_t{2});
    for (const auto &key : sensors)
    {
        const auto info = device.getSensorInfo(key);
        REQUIRE_EQ(info.key, key);
        REQUIRE(!info.name.empty());
        REQUIRE(!info.description.empty());
        // Metadata must not fabricate a current reading.
        REQUIRE(info.value.empty());
    }
}

TEST_CASE("temperature metadata describes signed integer Celsius readings and failure fallback")
{
    const auto info = SoapySidekiq{}.getSensorInfo("temperature");
    REQUIRE_EQ(info.type, SoapySDR::ArgInfo::INT);
    REQUIRE_EQ(info.units, std::string{"C"});
    REQUIRE(info.description.find("Signed") != std::string::npos);
    REQUIRE(info.description.find("Returns 0") != std::string::npos);
}

TEST_CASE("accelerometer metadata describes JSON axes and milli gravity units")
{
    const auto info = SoapySidekiq{}.getSensorInfo("accelerometer");
    REQUIRE_EQ(info.type, SoapySDR::ArgInfo::STRING);
    REQUIRE_EQ(info.units, std::string{"mg"});
    REQUIRE(info.description.find("JSON") != std::string::npos);
    REQUIRE(info.description.find("x, y, z") != std::string::npos);
    REQUIRE(info.description.find("thousandths of standard gravity") != std::string::npos);
    REQUIRE(info.description.find("Returns {}") != std::string::npos);
}

TEST_CASE("unknown sensor metadata keys raise descriptive argument errors")
{
    for (const std::string key : {"", "Temperature", "invalid"})
    {
        try
        {
            (void)SoapySidekiq{}.getSensorInfo(key);
            REQUIRE(false);
        }
        catch (const std::invalid_argument &error)
        {
            REQUIRE(std::string{error.what()}.find("Unknown sensor: " + key) != std::string::npos);
        }
    }
}
