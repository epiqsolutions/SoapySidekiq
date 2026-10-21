/**
 * @file TimeClock.cpp
 * @brief Implements PPS source selection, hardware time, and reference clocks.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/StringUtils.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
/** Kernel attribute shared by PPS source discovery, reporting, and selection. */
constexpr char KERNEL_1PPS_SOURCE_PATH[] = "/sys/kernel/epiq-axi-timing/pps/source";
/** Setting value when the kernel cannot report a recognized PPS source. */
constexpr char SOURCE_1PPS_UNAVAILABLE[] = "1pps_source_unavailable";
/** Setting value selecting the external PPS input. */
constexpr char SOURCE_1PPS_EXTERNAL[] = "1pps_source_external";
/** Setting value selecting the internal PPS source. */
constexpr char SOURCE_1PPS_INTERNAL[] = "1pps_source_internal";
} // namespace

/** Try kernel PPS source readback; return false when unavailable or unrecognized. */
bool readKernel1PpsSource(skiq_1pps_source_t &source)
{
    std::ifstream input(KERNEL_1PPS_SOURCE_PATH);
    if (!input.is_open())
    {
        return false;
    }

    std::string value;
    std::getline(input, value);
    if (!input.good() && !input.eof())
    {
        return false;
    }

    std::istringstream tokens(value);
    std::string token;
    std::string active_source;

    while (tokens >> token)
    {
        if (token.size() >= 2 && token.front() == '[' && token.back() == ']')
        {
            active_source = token.substr(1, token.size() - 2);
            break;
        }
    }

    if (active_source.empty())
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "no active kernel 1pps source found in '%s'",
                      value.c_str());
        return false;
    }

    if (active_source == "internal-gps")
    {
        source = skiq_1pps_source_host;
        return true;
    }

    if (active_source == "external")
    {
        source = skiq_1pps_source_external;
        return true;
    }

    if (active_source == "unavailable")
    {
        source = skiq_1pps_source_unavailable;
        return true;
    }
    SoapySDR_logf(SOAPY_SDR_WARNING, "unrecognized active kernel 1pps source '%s' from '%s'",
                  active_source.c_str(), value.c_str());
    return false;
}

/** Write and flush a supported kernel PPS source; return false on unavailable/rejected IO. */
bool writeKernel1PpsSource(const skiq_1pps_source_t source)
{
    const char *value = nullptr;

    switch (source)
    {
    case skiq_1pps_source_external:
        value = "external";
        break;

    case skiq_1pps_source_host:
        value = "internal-gps";
        break;

    default:
        return false;
    }

    std::ofstream output(KERNEL_1PPS_SOURCE_PATH);
    if (!output.is_open())
    {
        return false;
    }

    output << value;
    // Kernel endpoints can reject buffered writes only when the stream flushes.
    output.close();
    return output.good();
}

/*******************************************************************
 * Time API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listTimeSources(void) const
{
    std::vector<std::string> result;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listTimeSources");

    result.push_back(SOURCE_1PPS_UNAVAILABLE);
    result.push_back(SOURCE_1PPS_EXTERNAL);
    result.push_back(SOURCE_1PPS_INTERNAL);

    return result;
}

std::string SoapySidekiq::getTimeSource(void) const
{
    int status = 0;
    skiq_1pps_source_t pps_source = skiq_1pps_source_unavailable;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getTimeSource");

    status = skiq_read_1pps_source(card, &pps_source);
    if (status != 0)
    {
        if (readKernel1PpsSource(pps_source))
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "using kernel 1pps source fallback value %d for card %u",
                          pps_source, card);
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_1pps_source failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error(
                "skiq_read_1pps_source failed and kernel fallback unavailable: " +
                std::to_string(status));
        }
    }

    switch (pps_source)
    {
    case skiq_1pps_source_unavailable:
        return SOURCE_1PPS_UNAVAILABLE;
        break;

    case skiq_1pps_source_external:
        return SOURCE_1PPS_EXTERNAL;
        break;

    case skiq_1pps_source_host:
        return SOURCE_1PPS_INTERNAL;
        break;

    default:
        throw std::runtime_error("Invalid PPS source readback: " +
                                 std::to_string(static_cast<int>(pps_source)));
    }
}

/** Verify a PPS edge between two observations, including a zero baseline. */
void check_1pps(uint8_t card, skiq_1pps_source_t pps_source)
{
    if (pps_source != skiq_1pps_source_external && pps_source != skiq_1pps_source_host)
        return;
    uint64_t rf_timestamp = 0;
    uint64_t previous = 0;
    int status = skiq_read_last_1pps_timestamp(card, &rf_timestamp, &previous);
    if (status != 0)
        throw std::runtime_error("skiq_read_last_1pps_timestamp failed: " + std::to_string(status));
    if (usleep(1000000) != 0)
        throw std::runtime_error("PPS observation wait interrupted");
    uint64_t current = 0;
    status = skiq_read_last_1pps_timestamp(card, &rf_timestamp, &current);
    if (status != 0)
        throw std::runtime_error("skiq_read_last_1pps_timestamp failed: " + std::to_string(status));
    if (current == previous)
        throw std::runtime_error("No PPS edge observed for card " + std::to_string(card));
}

void SoapySidekiq::setTimeSource(const std::string &source)
{
    int status = 0;
    skiq_1pps_source_t pps_source = skiq_1pps_source_unavailable;

    SoapySDR_logf(SOAPY_SDR_TRACE, "setTimeSource");

    if (equalsIgnoreCase(source, SOURCE_1PPS_UNAVAILABLE))
    {
        pps_source = skiq_1pps_source_unavailable;
    }
    else if (equalsIgnoreCase(source, SOURCE_1PPS_EXTERNAL))
    {
        pps_source = skiq_1pps_source_external;
    }
    else if (equalsIgnoreCase(source, SOURCE_1PPS_INTERNAL))
    {
        pps_source = skiq_1pps_source_host;
    }
    else
    {
        throw std::invalid_argument("Unknown time source: " + source);
    }

    configureWhileRxInactive(
        [&]
        {
            status = skiq_write_1pps_source(card, pps_source);
            if (status != 0)
            {
                if (writeKernel1PpsSource(pps_source))
                {
                    SoapySDR_logf(SOAPY_SDR_INFO,
                                  "using kernel 1pps source write fallback value %d for card %u",
                                  pps_source, card);
                }
                else
                {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                                  "skiq_write_1pps_source %d failed, (card %u), status %d",
                                  pps_source, card, status);
                    throw std::runtime_error(
                        "skiq_write_1pps_source failed and kernel fallback unavailable: " +
                        std::to_string(status));
                }
            }

            SoapySDR_logf(SOAPY_SDR_INFO, "1pps source set to %s", source.c_str());

            check_1pps(card, pps_source);
        });

    return;
}

bool SoapySidekiq::hasHardwareTime(const std::string &what = "") const
{
    if (what.empty() || equalsIgnoreCase(what, "sys_timestamp"))
        return true;
    if (equalsIgnoreCase(what, "rx_rf_timestamp"))
        return num_rx_channels != 0;
    if (equalsIgnoreCase(what, "tx_rf_timestamp"))
        return num_tx_channels != 0;
    return false;
}

long long SoapySidekiq::getHardwareTime(const std::string &what = "") const
{
    uint64_t timestamp = 0;
    if (what.empty() || equalsIgnoreCase(what, "sys_timestamp"))
    {
        uint64_t frequency = 0;
        int status = skiq_read_sys_timestamp_freq(card, &frequency);
        if (status != 0)
            throw std::runtime_error("skiq_read_sys_timestamp_freq failed: " +
                                     std::to_string(status));
        if (frequency == 0)
            throw std::runtime_error("System timestamp frequency is missing or zero");
        status = skiq_read_curr_sys_timestamp(card, &timestamp);
        if (status != 0)
            throw std::runtime_error("skiq_read_curr_sys_timestamp failed: " +
                                     std::to_string(status));
        return soapy_sidekiq::ticksToNanoseconds(timestamp, frequency);
    }
    const bool rx = equalsIgnoreCase(what, "rx_rf_timestamp");
    const bool tx = equalsIgnoreCase(what, "tx_rf_timestamp");
    if (!rx && !tx)
        throw std::invalid_argument("Unknown hardware timestamp: " + what);
    validateChannel(rx ? SOAPY_SDR_RX : SOAPY_SDR_TX, 0);
    uint32_t configured_rate{}, configured_bw{}, actual_bw{};
    double actual_rate = std::numeric_limits<double>::quiet_NaN();
    // Read the current rate rather than an integer cache that can be stale or
    // omit fractional Hz. Bandwidth does not affect timestamp conversion.
    int status =
        rx ? skiq_read_rx_sample_rate_and_bandwidth(card, getFirstRxHandle(), &configured_rate,
                                                    &actual_rate, &configured_bw, &actual_bw)
           : skiq_read_tx_sample_rate_and_bandwidth(card, getFirstTxHandle(), &configured_rate,
                                                    &actual_rate, &configured_bw, &actual_bw);
    if (status != 0)
        throw std::runtime_error(std::string(rx ? "RX" : "TX") +
                                 " timestamp rate read failed: " + std::to_string(status));
    if (!std::isfinite(actual_rate) || actual_rate < 1 || actual_rate >= std::ldexp(1.0, 32))
        throw std::runtime_error("Invalid actual RF timestamp rate");
    status = rx ? skiq_read_curr_rx_timestamp(card, getFirstRxHandle(), &timestamp)
                : skiq_read_curr_tx_timestamp(card, getFirstTxHandle(), &timestamp);
    if (status != 0)
        throw std::runtime_error(
            std::string(rx ? "skiq_read_curr_rx_timestamp" : "skiq_read_curr_tx_timestamp") +
            " failed: " + std::to_string(status));
    return soapy_sidekiq::ticksToNanosecondsAtRate(timestamp, actual_rate);
}

void SoapySidekiq::setHardwareTime(const long long timeNs, const std::string &what = "")
{
    const bool now = what.empty() || equalsIgnoreCase(what, "now");
    const bool on_pps = equalsIgnoreCase(what, "1pps");
    if (!now && !on_pps)
        throw std::invalid_argument("Unknown hardware-time update: " + what);
    if (timeNs < 0)
        throw std::invalid_argument("Hardware time cannot be negative");
    configureWhileRxInactive(
        [&]
        {
            uint64_t frequency = 0;
            const int read_status = skiq_read_sys_timestamp_freq(card, &frequency);
            if (read_status != 0)
                throw std::runtime_error("skiq_read_sys_timestamp_freq failed: " +
                                         std::to_string(read_status));
            const auto timestamp = soapy_sidekiq::nanosecondsToTicks(timeNs, frequency);
            // Preserve SDK semantics: immediate updates set both counters to the same
            // raw ticks; PPS updates set RF to zero and system to the requested ticks.
            const int status = now ? skiq_update_timestamps(card, timestamp)
                                   : skiq_write_timestamp_update_on_1pps(card, 0, timestamp);
            if (status != 0)
                throw std::runtime_error(std::string(now ? "skiq_update_timestamps"
                                                         : "skiq_write_timestamp_update_on_1pps") +
                                         " failed: " + std::to_string(status));
        });
}

/*******************************************************************
 * Clocking API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listClockSources(void) const
{
    std::vector<std::string> result;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listClockSources");

    result.push_back("external_clock");
    result.push_back("internal_clock");

    return result;
}

std::string SoapySidekiq::getClockSource(void) const
{
    skiq_ref_clock_select_t source = skiq_ref_clock_invalid;
    const int status = skiq_read_ref_clock_select(card, &source);
    if (status != 0)
        throw std::runtime_error("skiq_read_ref_clock_select failed: " + std::to_string(status));
    if (source == skiq_ref_clock_internal || source == skiq_ref_clock_host)
        return "internal_clock";
    if (source == skiq_ref_clock_external)
        return "external_clock";
    throw std::runtime_error("Unsupported reference clock readback: " +
                             std::to_string(static_cast<int>(source)));
}

void SoapySidekiq::setClockSource(const std::string &source)
{
    skiq_ref_clock_select_t selected;
    if (equalsIgnoreCase(source, "internal_clock"))
        selected = skiq_ref_clock_internal;
    else if (equalsIgnoreCase(source, "external_clock"))
        selected = skiq_ref_clock_external;
    else
        throw std::invalid_argument("Unknown clock source: " + source);
    configureWhileRxInactive(
        [&]
        {
            const int status = skiq_write_ref_clock_select(card, selected);
            if (status != 0)
                throw std::runtime_error("skiq_write_ref_clock_select failed: " +
                                         std::to_string(status));
        });
}

double SoapySidekiq::getReferenceClockRate(void) const
{
    const std::string source = getClockSource();
    uint32_t frequency = 0;
    if (source == "external_clock")
    {
        const int status = skiq_read_ext_ref_clock_freq(card, &frequency);
        if (status != 0)
            throw std::runtime_error("skiq_read_ext_ref_clock_freq failed: " +
                                     std::to_string(status));
    }
    else
        frequency = param.rf_param.ref_clock_freq;
    if (frequency == 0)
        throw std::runtime_error("Reference clock frequency must be nonzero");
    return frequency;
}
