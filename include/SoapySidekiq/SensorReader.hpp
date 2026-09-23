#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace soapy_sidekiq
{

/** Hardware boundary for temperature and accelerometer operations. */
class SensorBackend
{
public:
    virtual ~SensorBackend() = default;

    /** Read the signed card temperature and return an SDK-style status. */
    virtual int readTemperature(std::int8_t &temperature) = 0;

    /** Query whether this card implements an accelerometer. */
    virtual int isAccelerometerSupported(bool &supported) = 0;

    /** Enable or disable accelerometer sampling. */
    virtual int setAccelerometerEnabled(bool enabled) = 0;

    /** Read all three accelerometer axes. */
    virtual int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) = 0;
};

/** Error that preserves both the failed operation and backend status code. */
class SensorBackendError : public std::runtime_error
{
public:
    /** Construct a descriptive error for one backend call. */
    SensorBackendError(std::string operation, int status);

    /** Return the logical operation that failed. */
    const std::string &operation() const noexcept;
    /** Return the original backend status code. */
    int status() const noexcept;

private:
    std::string operation_;
    int status_;
};

/** Signed raw accelerometer axes returned by Sidekiq. */
struct AccelerometerReading
{
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

/** Applies sensor lifecycle policy independently from the Sidekiq C API. */
class SensorReader
{
public:
    /** Reference a backend that must outlive this reader. */
    explicit SensorReader(SensorBackend &backend);

    /** Read a signed temperature or throw SensorBackendError. */
    std::int8_t readTemperature();

    /** Read a balanced enable/read/disable cycle, or nullopt if unsupported. */
    std::optional<AccelerometerReading> readAccelerometer();

private:
    SensorBackend &backend_;
};

/** Serialize an accelerometer reading as compact valid JSON. */
std::string formatAccelerometer(const AccelerometerReading &reading);

} // namespace soapy_sidekiq
