#include <SoapySidekiq/SensorReader.hpp>

#include <sstream>
#include <utility>

namespace soapy_sidekiq
{

SensorBackendError::SensorBackendError(std::string operation, const int status)
    : std::runtime_error(operation + " failed with status " + std::to_string(status)),
      operation_(std::move(operation)),
      status_(status)
{
}

const std::string &SensorBackendError::operation() const noexcept
{
    return operation_;
}

int SensorBackendError::status() const noexcept
{
    return status_;
}

SensorReader::SensorReader(SensorBackend &backend)
    : backend_(backend)
{
}

std::int8_t SensorReader::readTemperature()
{
    std::int8_t temperature = 0;
    const int status = backend_.readTemperature(temperature);
    if (status != 0)
    {
        throw SensorBackendError("read temperature", status);
    }
    return temperature;
}

std::optional<AccelerometerReading> SensorReader::readAccelerometer()
{
    bool supported = false;
    int status = backend_.isAccelerometerSupported(supported);
    if (status != 0)
    {
        throw SensorBackendError("query accelerometer support", status);
    }
    if (!supported)
    {
        return std::nullopt;
    }

    status = backend_.setAccelerometerEnabled(true);
    if (status != 0)
    {
        throw SensorBackendError("enable accelerometer", status);
    }

    AccelerometerReading reading;
    const int read_status = backend_.readAccelerometer(
        reading.x, reading.y, reading.z);
    const int disable_status = backend_.setAccelerometerEnabled(false);
    if (read_status != 0)
    {
        throw SensorBackendError("read accelerometer", read_status);
    }
    if (disable_status != 0)
    {
        throw SensorBackendError("disable accelerometer", disable_status);
    }
    return reading;
}

std::string formatAccelerometer(const AccelerometerReading &reading)
{
    std::ostringstream output;
    output << "{\"x\":" << reading.x
           << ",\"y\":" << reading.y
           << ",\"z\":" << reading.z << '}';
    return output.str();
}

} // namespace soapy_sidekiq
