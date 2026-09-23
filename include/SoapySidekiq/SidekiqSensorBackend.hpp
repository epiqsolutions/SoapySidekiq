#pragma once

#include <SoapySidekiq/SensorReader.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

class SidekiqSensorBackend final : public SensorBackend
{
public:
    explicit SidekiqSensorBackend(std::uint8_t card);

    int readTemperature(std::int8_t &temperature) override;
    int isAccelerometerSupported(bool &supported) override;
    int setAccelerometerEnabled(bool enabled) override;
    int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) override;

private:
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
