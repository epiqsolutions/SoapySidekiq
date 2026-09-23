#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{

class SensorBackend
{
public:
    virtual ~SensorBackend() = default;
    virtual int readTemperature(std::int8_t &temperature) = 0;
    virtual int isAccelerometerSupported(bool &supported) = 0;
    virtual int setAccelerometerEnabled(bool enabled) = 0;
    virtual int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) = 0;
};

class SensorBackendError : public std::runtime_error
{
public:
    SensorBackendError(std::string operation, int status);

    const std::string &operation() const noexcept;
    int status() const noexcept;

private:
    std::string operation_;
    int status_;
};

struct AccelerometerReading
{
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

class SensorReader
{
public:
    explicit SensorReader(SensorBackend &backend);

    std::int8_t readTemperature();
    std::optional<AccelerometerReading> readAccelerometer();

private:
    SensorBackend &backend_;
};

std::string formatAccelerometer(const AccelerometerReading &reading);

} // namespace soapy_sidekiq
