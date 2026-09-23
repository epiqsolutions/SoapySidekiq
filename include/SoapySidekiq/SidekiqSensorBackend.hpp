#pragma once

#include <SoapySidekiq/SensorReader.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

/** Adapts Sidekiq sensor calls to the hardware-independent SensorBackend. */
class SidekiqSensorBackend final : public SensorBackend
{
public:
    /** Bind sensor operations to one initialized Sidekiq card. */
    explicit SidekiqSensorBackend(std::uint8_t card);

    /** Forward a signed temperature read to the SDK. */
    int readTemperature(std::int8_t &temperature) override;
    /** Forward the accelerometer support query to the SDK. */
    int isAccelerometerSupported(bool &supported) override;
    /** Forward accelerometer enable state to the SDK. */
    int setAccelerometerEnabled(bool enabled) override;
    /** Forward a three-axis accelerometer read to the SDK. */
    int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) override;

private:
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
