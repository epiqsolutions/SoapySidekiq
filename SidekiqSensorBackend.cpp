#include <SoapySidekiq/SidekiqSensorBackend.hpp>

#include <sidekiq_api.h>

namespace soapy_sidekiq
{

SidekiqSensorBackend::SidekiqSensorBackend(const std::uint8_t card)
    : card_(card)
{
}

int SidekiqSensorBackend::readTemperature(std::int8_t &temperature)
{
    return skiq_read_temp(card_, &temperature);
}

int SidekiqSensorBackend::isAccelerometerSupported(bool &supported)
{
    return skiq_is_accel_supported(card_, &supported);
}

int SidekiqSensorBackend::setAccelerometerEnabled(const bool enabled)
{
    return skiq_write_accel_state(card_, enabled ? 1 : 0);
}

int SidekiqSensorBackend::readAccelerometer(
    std::int16_t &x, std::int16_t &y, std::int16_t &z)
{
    return skiq_read_accel(card_, &x, &y, &z);
}

} // namespace soapy_sidekiq
