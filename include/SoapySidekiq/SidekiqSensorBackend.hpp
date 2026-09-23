/**
 * @file SidekiqSensorBackend.hpp
 * @brief Declares the Sidekiq SDK adapter for portable sensor policy.
 */

#pragma once

#include <SoapySidekiq/SensorReader.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

/** Adapts Sidekiq sensor calls to the hardware-independent SensorBackend. */
class SidekiqSensorBackend final : public SensorBackend
{
public:
    /**
     * Bind sensor operations to one initialized Sidekiq card.
     * @param card SDK card index used by every backend operation.
     */
    explicit SidekiqSensorBackend(std::uint8_t card);

    /** @copydoc SensorBackend::readTemperature */
    int readTemperature(std::int8_t &temperature) override;
    /** @copydoc SensorBackend::isAccelerometerSupported */
    int isAccelerometerSupported(bool &supported) override;
    /** @copydoc SensorBackend::setAccelerometerEnabled */
    int setAccelerometerEnabled(bool enabled) override;
    /** @copydoc SensorBackend::readAccelerometer */
    int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) override;

private:
    /** Initialized SDK card index owned by the surrounding device. */
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
