/**
 * @file Settings.cpp
 * @brief Implements device settings metadata, validation, and SDK transactions.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/StringUtils.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

/*******************************************************************
 * Settings API
 ******************************************************************/

SoapySDR::ArgInfoList SoapySidekiq::getSettingInfo(void) const
{
    SoapySDR::ArgInfoList setArgs;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getSettingInfo");

    SoapySDR::ArgInfo settingArg;

    settingArg.key = "iq_swap";
    settingArg.value = iq_swap ? "true" : "false";
    settingArg.name = "I/Q Swap";
    settingArg.description = "Set I then Q format";
    settingArg.type = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key = "counter";
    settingArg.value = counter ? "true" : "false";
    settingArg.name = "counter";
    settingArg.description = "FPGA creates RAMP samples";
    settingArg.type = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key = "sys_clock_freq";
    settingArg.value = std::to_string(sys_freq);
    settingArg.name = "sys_clock_freq";
    settingArg.description = "Read-only frequency of the system timestamp clock";
    settingArg.type = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.options.clear();
    settingArg.key = "full_scale";
    settingArg.value = std::to_string(static_cast<uint32_t>(this->maxValue));
    settingArg.name = "full_scale";
    settingArg.description = "Read-only device-specific full-scale integer sample value";
    settingArg.type = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.key = "timetype";
    settingArg.value = timetype;
    settingArg.name = "timetype";
    settingArg.description = "The type of timestamp to return in readStream";
    settingArg.type = SoapySDR::ArgInfo::STRING;
    settingArg.options.push_back("rf_timestamp");
    settingArg.options.push_back("sys_timestamp");
    setArgs.push_back(settingArg);

    return setArgs;
}

namespace
{
/** Parse documented boolean forms before any device or cached-state changes. */
bool parseSettingBool(const std::string &key, const std::string &value)
{
    if (equalsIgnoreCase(value, "true") || value == "1")
        return true;
    if (equalsIgnoreCase(value, "false") || value == "0")
        return false;
    throw std::invalid_argument("Setting " + key + " requires true, false, 1, or 0");
}
} // namespace

void SoapySidekiq::writeSetting(const std::string &key, const std::string &value)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "writeSetting");
    if (equalsIgnoreCase(key, "iq_swap"))
    {
        const bool requested = parseSettingBool(key, value);
        const int status =
            skiq_write_iq_order_mode(card, requested ? skiq_iq_order_iq : skiq_iq_order_qi);
        if (status != 0)
            throw std::runtime_error("skiq_write_iq_order_mode failed: " + std::to_string(status));
        iq_swap = requested;
    }
    else if (equalsIgnoreCase(key, "counter"))
    {
        const bool requested = parseSettingBool(key, value);
        if (num_rx_channels == 0)
            throw std::runtime_error("Counter setting requires an RX channel");
        const auto source = requested ? skiq_data_src_counter : skiq_data_src_iq;
        // Snapshot every mapped handle before writes; allocate rollback storage first.
        std::vector<skiq_rx_hdl_t> handles;
        std::vector<skiq_data_src_t> previous;
        handles.reserve(num_rx_channels);
        previous.reserve(num_rx_channels);
        for (size_t channel = 0; channel < num_rx_channels; ++channel)
        {
            const auto handle = getRxHandle(channel);
            skiq_data_src_t old_source = skiq_data_src_iq;
            const int status = skiq_read_rx_data_src(card, handle, &old_source);
            if (status != 0)
                throw std::runtime_error("skiq_read_rx_data_src failed on channel " +
                                         std::to_string(channel) + ": " + std::to_string(status));
            if (old_source != skiq_data_src_iq && old_source != skiq_data_src_counter)
                throw std::runtime_error("Invalid RX data source on channel " +
                                         std::to_string(channel));
            handles.push_back(handle);
            previous.push_back(old_source);
        }
        for (size_t channel = 0; channel < handles.size(); ++channel)
        {
            const int status = skiq_write_rx_data_src(card, handles[channel], source);
            if (status != 0)
            {
                // Include the failed write: an SDK error can follow a partial update.
                // Finish all compensation attempts before constructing the diagnostic.
                int rollback_status = 0;
                for (size_t attempted = channel + 1; attempted > 0; --attempted)
                {
                    const int restored = skiq_write_rx_data_src(card, handles[attempted - 1],
                                                                previous[attempted - 1]);
                    if (restored != 0 && rollback_status == 0)
                        rollback_status = restored;
                }
                std::string message = "skiq_write_rx_data_src failed on channel " +
                                      std::to_string(channel) + ": " + std::to_string(status);
                if (rollback_status != 0)
                    message +=
                        "; RX data-source rollback failed: " + std::to_string(rollback_status);
                throw std::runtime_error(message);
            }
        }
        counter = requested;
    }
    else if (equalsIgnoreCase(key, "timetype"))
    {
        if (value != "rf_timestamp" && value != "sys_timestamp")
            throw std::invalid_argument("Invalid timetype: " + value);
        timetype = value;
        rfTimeSource = value == "rf_timestamp";
    }
    else if (equalsIgnoreCase(key, "sys_clock_freq") || equalsIgnoreCase(key, "full_scale") ||
             equalsIgnoreCase(key, "overload") || equalsIgnoreCase(key, "cal_offset"))
        throw std::invalid_argument("Read-only setting: " + key);
    else
        throw std::invalid_argument("Unknown setting: " + key);
}

std::string SoapySidekiq::readSetting(const std::string &key) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "readSetting");

    if (equalsIgnoreCase(key, "iq_swap"))
    {
        return iq_swap ? "true" : "false";
    }
    else if (equalsIgnoreCase(key, "counter"))
    {
        return counter ? "true" : "false";
    }
    else if (equalsIgnoreCase(key, "timetype"))
    {
        return timetype;
    }
    else if (equalsIgnoreCase(key, "sys_clock_freq"))
    {
        return std::to_string(static_cast<uint64_t>(this->sys_freq));
    }
    else if (equalsIgnoreCase(key, "full_scale"))
    {
        return std::to_string(static_cast<uint32_t>(this->maxValue));
    }
    else if (equalsIgnoreCase(key, "overload"))
    {
        uint8_t overload_state = 0;
        int32_t status =
            skiq_read_rx_overload_state(this->card, getFirstRxHandle(), &overload_state);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_overload_state failed, card: %u status: %d", this->card,
                          status);
            throw std::runtime_error("skiq_read_rx_overload_state failed (card " +
                                     std::to_string(card) + ", channel 0, handle " +
                                     std::to_string(getFirstRxHandle()) + ", status " +
                                     std::to_string(status) + ")");
        }
        return std::to_string(overload_state);
    }
    else if (equalsIgnoreCase(key, "cal_offset"))
    {
        double cal_offset = 0;
        int32_t status = skiq_read_rx_cal_offset(this->card, getFirstRxHandle(), &cal_offset);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_cal_offset failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("skiq_read_rx_cal_offset failed (card " +
                                     std::to_string(card) + ", channel 0, handle " +
                                     std::to_string(getFirstRxHandle()) + ", status " +
                                     std::to_string(status) + ")");
        }
        return std::to_string(cal_offset);
    }
    else
    {
        throw std::invalid_argument("Unknown setting: " + key);
    }
    return "";
}
