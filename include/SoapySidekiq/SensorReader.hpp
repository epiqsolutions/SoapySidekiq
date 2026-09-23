/**
 * @file SensorReader.hpp
 * @brief Declares hardware-independent sensor lifecycle and formatting policy.
 */

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

    /**
     * Read the signed card temperature.
     * @param temperature Receives the temperature in degrees Celsius.
     * @return SDK-style status code; zero indicates success.
     */
    virtual int readTemperature(std::int8_t &temperature) = 0;

    /**
     * Query whether this card implements an accelerometer.
     * @param supported Receives the hardware capability state.
     * @return SDK-style status code; zero indicates success.
     */
    virtual int isAccelerometerSupported(bool &supported) = 0;

    /**
     * Enable or disable accelerometer sampling.
     * @param enabled Requested sampling state.
     * @return SDK-style status code; zero indicates success.
     */
    virtual int setAccelerometerEnabled(bool enabled) = 0;

    /**
     * Read all three accelerometer axes.
     * @param x Receives the signed X-axis value.
     * @param y Receives the signed Y-axis value.
     * @param z Receives the signed Z-axis value.
     * @return SDK-style status code; zero indicates success.
     */
    virtual int readAccelerometer(
        std::int16_t &x, std::int16_t &y, std::int16_t &z) = 0;
};

/** Error that preserves both the failed operation and backend status code. */
class SensorBackendError : public std::runtime_error
{
public:
    /**
     * Construct a descriptive error for one backend call.
     * @param operation Logical operation that failed.
     * @param status Original backend status code.
     */
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
    /**
     * Reference a backend that must outlive this reader.
     * @param backend Sensor backend used for subsequent reads.
     */
    explicit SensorReader(SensorBackend &backend);

    /**
     * Read a signed temperature.
     * @return Temperature in degrees Celsius.
     * @throws SensorBackendError when the backend read fails.
     */
    std::int8_t readTemperature();

    /**
     * Read a balanced enable/read/disable accelerometer cycle.
     * @return Three-axis reading, or nullopt when the hardware lacks the sensor.
     * @throws SensorBackendError when capability, enable, read, or cleanup fails.
     */
    std::optional<AccelerometerReading> readAccelerometer();

private:
    SensorBackend &backend_;
};

/**
 * Serialize an accelerometer reading as compact valid JSON.
 * @param reading Signed axes to serialize.
 * @return JSON object containing x, y, and z numeric members.
 */
std::string formatAccelerometer(const AccelerometerReading &reading);

} // namespace soapy_sidekiq
