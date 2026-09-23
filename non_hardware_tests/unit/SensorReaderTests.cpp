/**
 * @file SensorReaderTests.cpp
 * @brief Verifies signed readings, lifecycle cleanup, errors, and JSON output.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/SensorReader.hpp>

#include <cstdint>
#include <string>
#include <vector>

using soapy_sidekiq::AccelerometerReading;
using soapy_sidekiq::SensorBackend;
using soapy_sidekiq::SensorBackendError;
using soapy_sidekiq::SensorReader;

namespace
{

/** Records call order and scripts each sensor backend status independently. */
class FakeSensorBackend final : public SensorBackend
{
public:
    /** @copydoc SensorBackend::readTemperature */
    int readTemperature(std::int8_t &value) override
    {
        calls.push_back("temperature");
        value = temperature;
        return temperature_status;
    }

    /** @copydoc SensorBackend::isAccelerometerSupported */
    int isAccelerometerSupported(bool &value) override
    {
        calls.push_back("supported");
        value = supported;
        return support_status;
    }

    /** @copydoc SensorBackend::setAccelerometerEnabled */
    int setAccelerometerEnabled(const bool enabled) override
    {
        calls.push_back(enabled ? "enable" : "disable");
        return enabled ? enable_status : disable_status;
    }

    /** @copydoc SensorBackend::readAccelerometer */
    int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) override
    {
        calls.push_back("read");
        x = reading.x;
        y = reading.y;
        z = reading.z;
        return read_status;
    }

    std::int8_t temperature{25};
    AccelerometerReading reading{1, -2, 3};
    bool supported{true};
    int temperature_status{};
    int support_status{};
    int enable_status{};
    int read_status{};
    int disable_status{};
    std::vector<std::string> calls;
};

} // namespace

TEST_CASE("temperature readings preserve signed values")
{
    FakeSensorBackend backend;
    backend.temperature = -12;
    SensorReader reader(backend);
    REQUIRE_EQ(reader.readTemperature(), std::int8_t{-12});
    REQUIRE(backend.calls == std::vector<std::string>{"temperature"});
}

TEST_CASE("temperature backend errors retain operation and status")
{
    FakeSensorBackend backend;
    backend.temperature_status = -5;
    SensorReader reader(backend);
    try
    {
        (void)reader.readTemperature();
        REQUIRE(false);
    }
    catch (const SensorBackendError &error)
    {
        REQUIRE_EQ(error.operation(), std::string{"read temperature"});
        REQUIRE_EQ(error.status(), -5);
    }
}

TEST_CASE("unsupported accelerometers are not enabled")
{
    FakeSensorBackend backend;
    backend.supported = false;
    SensorReader reader(backend);
    REQUIRE(!reader.readAccelerometer().has_value());
    REQUIRE(backend.calls == std::vector<std::string>{"supported"});
}

TEST_CASE("accelerometer reads use a balanced enable lifecycle")
{
    FakeSensorBackend backend;
    SensorReader reader(backend);
    const auto result = reader.readAccelerometer();
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->x, std::int16_t{1});
    REQUIRE_EQ(result->y, std::int16_t{-2});
    REQUIRE_EQ(result->z, std::int16_t{3});
    REQUIRE((backend.calls ==
        std::vector<std::string>{"supported", "enable", "read", "disable"}));
}

TEST_CASE("accelerometer support and enable failures stop the lifecycle")
{
    FakeSensorBackend support_failure;
    support_failure.support_status = -2;
    SensorReader support_reader(support_failure);
    REQUIRE_THROWS_AS(support_reader.readAccelerometer(), SensorBackendError);
    REQUIRE(support_failure.calls == std::vector<std::string>{"supported"});

    FakeSensorBackend enable_failure;
    enable_failure.enable_status = -3;
    SensorReader enable_reader(enable_failure);
    REQUIRE_THROWS_AS(enable_reader.readAccelerometer(), SensorBackendError);
    REQUIRE((enable_failure.calls ==
        std::vector<std::string>{"supported", "enable"}));
}

TEST_CASE("accelerometer read failures still disable the sensor")
{
    FakeSensorBackend backend;
    backend.read_status = -7;
    SensorReader reader(backend);
    try
    {
        (void)reader.readAccelerometer();
        REQUIRE(false);
    }
    catch (const SensorBackendError &error)
    {
        REQUIRE_EQ(error.operation(), std::string{"read accelerometer"});
        REQUIRE_EQ(error.status(), -7);
    }
    REQUIRE((backend.calls ==
        std::vector<std::string>{"supported", "enable", "read", "disable"}));
}

TEST_CASE("accelerometer disable failures are reported")
{
    FakeSensorBackend backend;
    backend.disable_status = -8;
    SensorReader reader(backend);
    try
    {
        (void)reader.readAccelerometer();
        REQUIRE(false);
    }
    catch (const SensorBackendError &error)
    {
        REQUIRE_EQ(error.operation(), std::string{"disable accelerometer"});
        REQUIRE_EQ(error.status(), -8);
    }
}

TEST_CASE("accelerometer readings format as valid compact JSON")
{
    REQUIRE_EQ(
        soapy_sidekiq::formatAccelerometer({12, -34, 56}),
        std::string{"{\"x\":12,\"y\":-34,\"z\":56}"});
}
