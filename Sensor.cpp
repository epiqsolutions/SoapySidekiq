#include "SoapySidekiq.hpp"

#include <SoapySidekiq/SensorReader.hpp>
#include <SoapySidekiq/SidekiqSensorBackend.hpp>

std::vector<std::string> SoapySidekiq::listSensors(void) const
{
    SoapySDR_log(SOAPY_SDR_TRACE, "listSensors");
    return {"temperature", "accelerometer"};
}

std::string SoapySidekiq::readSensor(const std::string &key) const
{
    SoapySDR_log(SOAPY_SDR_TRACE, "readSensor");
    // Keep the Soapy-facing method thin; SensorReader owns lifecycle policy.
    soapy_sidekiq::SidekiqSensorBackend backend(card);
    soapy_sidekiq::SensorReader reader(backend);

    try
    {
        if (key == "temperature")
        {
            const auto temperature = reader.readTemperature();
            SoapySDR_logf(SOAPY_SDR_DEBUG, "Temp is %d", temperature);
            return std::to_string(temperature);
        }
        if (key == "accelerometer")
        {
            const auto reading = reader.readAccelerometer();
            if (!reading.has_value())
            {
                SoapySDR_logf(SOAPY_SDR_WARNING,
                    "Accelerometer not supported by card %u", card);
                return "{}";
            }
            const auto formatted = soapy_sidekiq::formatAccelerometer(*reading);
            SoapySDR_logf(SOAPY_SDR_DEBUG,
                "accel data %s", formatted.c_str());
            return formatted;
        }
    }
    catch (const soapy_sidekiq::SensorBackendError &error)
    {
        // Preserve the legacy string-returning API while logging full diagnostics.
        SoapySDR_logf(SOAPY_SDR_ERROR,
            "Sensor operation '%s' failed (card %u), status %d",
            error.operation().c_str(), card, error.status());
        return key == "temperature" ? "0" : "{}";
    }

    return SoapySDR::Device::readSensor(key);
}
