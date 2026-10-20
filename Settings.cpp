/**
 * @file Settings.cpp
 * @brief Implements Sidekiq device settings, callbacks, and utility operations.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/DeviceOptions.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/SidekiqRxBackend.hpp>
#include <SoapySidekiq/SampleScale.hpp>
#include <SoapySidekiq/NumericConversion.hpp>
#include <SoapySidekiq/TimestampConversion.hpp>
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cinttypes>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <sidekiq_types.h>
#include <unistd.h>

// Keep the hardware-independent validation contract tied to the SDK in use.
static_assert(soapy_sidekiq::maxTxBlockSize == SKIQ_MAX_TX_BLOCK_SIZE_IN_WORDS);
static_assert(soapy_sidekiq::txHeaderWords == SKIQ_TX_HEADER_SIZE_IN_WORDS);
static_assert(soapy_sidekiq::txPacketIncrementWords == SKIQ_TX_PACKET_SIZE_INCREMENT_IN_WORDS);

/**
 * Forward an SDK diagnostic to SoapySDR without taking ownership of its text.
 * @param priority SDK log priority, mapped to the corresponding Soapy level.
 * @param message Borrowed, null-terminated SDK text; null is treated as empty.
 *
 * Remove at most one trailing LF and then one CR, preserving the historical
 * line-ending behavior. Diagnostics must not propagate C++ exceptions through
 * the SDK's C callback boundary, even if allocation or a log sink fails.
 */
void logging_handler(int32_t priority, const char *message) noexcept
{
    try
    {
        std::string text(message != nullptr ? message : "");
        if (!text.empty() && text.back() == '\n')
        {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '\r')
        {
            text.pop_back();
        }

        switch (priority)
        {
        case SKIQ_LOG_DEBUG:
            SoapySDR_logf(SOAPY_SDR_DEBUG, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_INFO:
            SoapySDR_logf(SOAPY_SDR_INFO, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_WARNING:
            SoapySDR_logf(SOAPY_SDR_WARNING, "epiq-log: %s", text.c_str());
            break;
        case SKIQ_LOG_ERROR:
            SoapySDR_logf(SOAPY_SDR_ERROR, "epiq-log: %s", text.c_str());
            break;
        default:
            SoapySDR_logf(SOAPY_SDR_TRACE, "epiq-log undefined %s", text.c_str());
        }
    }
    catch (...)
    {
        // Drop a failed diagnostic. Retrying the sink could recurse or throw
        // again while the SDK is already reporting an error.
    }
}

/*****************************************************************************/
  /** This is the callback function for once the data has completed being sent.
      There is no guarantee that the complete callback will be in the order that
      the data was sent, this function just increments the completion count and
      signals the main thread that there is space available to send more packets.

      @param status status of the transmit packet completed
      @param p_block reference to the completed transmit block
      @param p_user reference to the user data
      @return void
 */
void SoapySidekiq::tx_complete(int32_t status, skiq_tx_block_t *p_data, uint32_t txIndex)
{
    if (txIndex >= DEFAULT_NUM_BUFFERS || !p_tx_status)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received invalid buffer index %u", txIndex);
        return;
    }
    // update the in use status of the packet just completed
    bool unexpected_status;
    {
        std::lock_guard<std::mutex> lock(tx_buf_mutex);
        unexpected_status = p_tx_status[txIndex] != 1;
        p_tx_status[txIndex] = 0;
        ++complete_count;
    }
    tx_buf_cv.notify_all();
    // Publish completion before diagnostics, and never call a log sink with
    // the status mutex held: user logging code may throw or re-enter.
    if (unexpected_status)
        SoapySDR_logf(SOAPY_SDR_ERROR, "TX completion received a buffer that was not reserved");
}


// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    return std::equal(a.begin(), a.end(), b.begin(),
        [](char lhs, char rhs)
         {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
         });
}

namespace
{
// Kernel PPS names are shared by source discovery, reporting, and selection.
constexpr char KERNEL_1PPS_SOURCE_PATH[] = "/sys/kernel/epiq-axi-timing/pps/source";
constexpr char SOURCE_1PPS_UNAVAILABLE[] = "1pps_source_unavailable";
constexpr char SOURCE_1PPS_EXTERNAL[] = "1pps_source_external";
constexpr char SOURCE_1PPS_INTERNAL[] = "1pps_source_internal";
} // namespace

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
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "no active kernel 1pps source found in '%s'",
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

    if (active_source != "unavailable")
    {
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "unrecognized active kernel 1pps source '%s' from '%s'",
                      active_source.c_str(), value.c_str());
    }
    return false;
}

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
    return output.good();
}

void SoapySidekiq::validateChannel(const int direction, const size_t channel) const
{
    const auto count = getNumChannels(direction);
    if (channel >= count)
    {
        throw std::out_of_range(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") + " channel " +
                                std::to_string(channel) + " is unavailable; device has " +
                                std::to_string(count) + " channel(s)");
    }
}

skiq_rx_hdl_t SoapySidekiq::getRxHandle(const size_t channel) const
{
    validateChannel(SOAPY_SDR_RX, channel);
    return this->param.rx_param[channel].handle;
}

skiq_tx_hdl_t SoapySidekiq::getTxHandle(const size_t channel) const
{
    validateChannel(SOAPY_SDR_TX, channel);
    return this->param.tx_param[channel].handle;
}

skiq_rx_hdl_t SoapySidekiq::getFirstRxHandle(void) const
{
    return getRxHandle(0);
}

skiq_tx_hdl_t SoapySidekiq::getFirstTxHandle(void) const
{
    return getTxHandle(0);
}

// Acquire constructor callback resources as one transaction. No stream can be
// active yet, so failed registration can be unwound before SDK initialization
// is released by the constructor's outer rollback guard.
void SoapySidekiq::initializeTxCallbacks()
{
    registerInstance(card, this);
    soapy_sidekiq::ScopeExit registry_rollback([this]() noexcept
                                               { unregisterInstance(card, this); });

    const int status =
        skiq_register_tx_complete_callback(card, &SoapySidekiq::tx_complete_callback);
    if (status != 0)
    {
        throw std::runtime_error("skiq_register_tx_complete_callback failed (card " +
                                 std::to_string(card) + ", status " + std::to_string(status) + ")");
    }
    soapy_sidekiq::ScopeExit callback_rollback(
        [this]() noexcept { skiq_register_tx_complete_callback(card, nullptr); });

    // A user-installed log sink may throw. Keep rollback armed through the
    // final potentially throwing operation in the device constructor.
    SoapySDR_logf(SOAPY_SDR_TRACE, "leaving constructor", card);
    callback_rollback.release();
    registry_rollback.release();
}

// Constructor
SoapySidekiq::SoapySidekiq(const SoapySDR::Kwargs &args)
{
    int status = 0;
    uint8_t channels = 0;
    skiq_iq_order_t iq_order;
    // Reject all malformed constructor input before registering or opening SDK state.
    const auto options = soapy_sidekiq::parseDeviceOptions(args);

    /* Register our own logging function before initializing the library */
    skiq_register_logging( logging_handler );

    SoapySDR_logf(SOAPY_SDR_TRACE, "in constructor", card);

    /* We need to set some default parameters in case the user does not */
    rxUseShort  = true;
    txUseShort  = true;
    iq_swap   = true;
    counter   = false;
    debug_ctr = 0;
    card      = 0;
    rx_block_size_in_words = 0;
    rx_block_size_in_bytes = 0;
    rx_payload_size_in_words = 1018;
    rx_payload_size_in_bytes = 0;
    rfTimeSource = true;
    timetype = "rf_timestamp";
    complete_count = 0;

    card = options.card;
    current_tx_block_size = options.tx_block_size;
    SoapySDR_logf(SOAPY_SDR_INFO, "TX block size set to %u", current_tx_block_size);

    skiq_xport_type_t type  = skiq_xport_type_auto;
    skiq_xport_init_level_t level = skiq_xport_init_level_full;

    SoapySDR_logf(SOAPY_SDR_INFO, "Sidekiq opening card %u", card);

    /* init sidekiq */
    status = skiq_init(type, level, &card, 1);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_init failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    // A throwing constructor never runs the device destructor. Only arm this
    // rollback after this call successfully acquires SDK initialization.
    soapy_sidekiq::ScopeExit rollback(
        [this]() noexcept
        {
            p_tx_status.reset();
            skiq_exit();
        });
    // The backend owns no hardware resource; the session owns its worker.
    rx_backend = std::make_unique<soapy_sidekiq::SidekiqRxBackend>(card);
    rx_session = std::make_unique<soapy_sidekiq::RxStreamSession>(*rx_backend);

    if (options.topology.has_value())
    {
        if (skiq_is_topology_supported(card))
        {
            status = skiq_apply_topology(card, *options.topology);
            if (status != 0)
            {
                SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_apply_topology failed (card %u), status %d",
                              card, status);
                throw std::runtime_error("");
            }
        }
	else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Topology not supported on card %u; ignoring request",
                          card, status);
        }
    }

    status = skiq_read_parameters(card, &this->param);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_parameters failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("");
    }

    status = skiq_read_sys_timestamp_freq(card, &sys_freq);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_sys_timestamp_freq failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    part = param.card_param.part_type;
    part_str = skiq_part_string(part);

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, part type is %s", card, part_str.c_str());

    /* set iq order to iq instead of qi */
    if (iq_swap)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to I then Q", this->card);
        iq_order = skiq_iq_order_iq;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, setting iq mode to Q then I", this->card);
        iq_order = skiq_iq_order_qi;
    }

    status = skiq_write_iq_order_mode(card, iq_order);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_iq_order_mode failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }

    status = skiq_read_num_rx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_rx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    num_rx_channels = channels;

    for (size_t chan = 0; chan < num_rx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy RX channel %zu maps to Sidekiq rx handle %u",
                      chan, getRxHandle(chan));
    }

    status = skiq_read_num_tx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_tx_chans failed, (card %u), status %d",
                      card, status);
        throw std::runtime_error("");
    }
    num_tx_channels = channels;
    for (size_t chan = 0; chan < num_tx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy TX channel %zu maps to Sidekiq tx handle %u",
                      chan, getTxHandle(chan));
    }

    uint8_t tmp_resolution = 0;

    /* Every card can have a different iq resolution.  */
    status = skiq_read_rx_iq_resolution(card, &tmp_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_rx_iq_resolution failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("");
    }

    this->resolution = tmp_resolution;
    this->maxValue = soapy_sidekiq::sampleFullScale(tmp_resolution);

    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, card resolution: %u bits, max ADC value: %u", card,
                  this->resolution, static_cast<uint32_t>(this->maxValue));

    // The transport stores signed 16-bit components, but DAC width is product-specific.
    uint8_t tx_resolution = 0;
    status = skiq_read_tx_iq_resolution(card, &tx_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_tx_iq_resolution failed, card: %u status: %d",
                      card, status);
        throw std::runtime_error("");
    }
    // The SDK manual lists these products as upper-14-bit TX. Their low two
    // host bits are discarded, so DAC resolution is not the host amplitude width.
    const auto tx_alignment = (part == skiq_x2 || part == skiq_x4 || part == skiq_x40)
        ? soapy_sidekiq::TxSampleAlignment::upper_bits
        : soapy_sidekiq::TxSampleAlignment::right_justified;
    this->txMaxValue = soapy_sidekiq::txSampleFullScale(tx_resolution, tx_alignment);
    SoapySDR_logf(SOAPY_SDR_INFO,
                  "card: %u, TX DAC resolution: %u bits, host full scale: %u",
                  card, tx_resolution, static_cast<uint32_t>(this->txMaxValue));

    status = skiq_write_chan_mode(card, skiq_chan_mode_single);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_chan_mode failed, card %u, status %d",
                      card, status);
        throw std::runtime_error("");
    }
    SoapySDR_logf(SOAPY_SDR_TRACE, "channel mode set to single");

    /* cache the current sample rate and bandwidth */
    rx_sample_rates.resize(num_rx_channels, 0);
    rx_bandwidths.resize(num_rx_channels, 0);
    for (size_t chan = 0; chan < num_rx_channels; chan++)
    {
        uint32_t configured_rate = 0;
        double actual_rate = 0;
        uint32_t configured_bw = 0;
        uint32_t actual_bandwidth = 0;

        status = skiq_read_rx_sample_rate_and_bandwidth(card,
                                                        getRxHandle(chan),
                                                        &configured_rate,
                                                        &actual_rate,
                                                        &configured_bw,
                                                        &actual_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_sample_rate_and_bandwidth failed (card %u, channel %zu), status %d",
                          card, chan, status);
            throw std::runtime_error("");
        }

        rx_sample_rates[chan] = static_cast<uint32_t>(actual_rate);
        rx_bandwidths[chan] = actual_bandwidth;
    }

    tx_sample_rates.resize(num_tx_channels, 0);
    tx_bandwidths.resize(num_tx_channels, 0);
    for (size_t chan = 0; chan < num_tx_channels; chan++)
    {
        uint32_t configured_rate = 0;
        double actual_rate = 0;
        uint32_t configured_bw = 0;
        uint32_t actual_bandwidth = 0;

        status = skiq_read_tx_sample_rate_and_bandwidth(card,
                                                        getTxHandle(chan),
                                                        &configured_rate,
                                                        &actual_rate,
                                                        &configured_bw,
                                                        &actual_bandwidth);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_sample_rate_and_bandwidth failed (card %u, channel %zu), status %d",
                          card, chan, status);
            throw std::runtime_error("");
        }

        tx_sample_rates[chan] = static_cast<uint32_t>(actual_rate);
        tx_bandwidths[chan] = actual_bandwidth;
    }

    // Source setters require initialized hardware, so apply validated names last.
    if (options.clock_source.has_value())
    {
        setClockSource(*options.clock_source);
    }

    if (options.time_source.has_value())
    {
        setTimeSource(*options.time_source);
    }

    // Value-initialize every entry before making storage visible to callbacks.
    // The member owner also releases it if later constructor work throws.
    p_tx_status = std::make_unique<int32_t[]>(DEFAULT_NUM_BUFFERS);

    initializeTxCallbacks();
    rollback.release();
}

// Destructor
SoapySidekiq::~SoapySidekiq(void)
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "In destructor", card);

    // Stop and join RX before invalidating callbacks or exiting the SDK.
    if (rx_session != nullptr)
    {
        rx_session->shutdown();
    }
    rx_session.reset();
    rx_backend.reset();
    delete active_rx_stream;
    active_rx_stream = nullptr;

    unregisterInstance(card, this);
    skiq_register_tx_complete_callback(card, nullptr);

    p_tx_status.reset();

    skiq_exit();
}

/*******************************************************************
 * Identification API
 ******************************************************************/

std::string SoapySidekiq::getDriverKey(void) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getDriverKey");
    return "Sidekiq";
}
std::string SoapySidekiq::getHardwareKey(void) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareKey");
    return part_str;
}



SoapySDR::Kwargs SoapySidekiq::getHardwareInfo(void) const
{
    //  key/value pairs for any useful information
    //  this also gets printed in --probe
    SoapySDR::Kwargs args;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareInfo");

    args["origin"] = "https://github.com/epiqsolutions/SoapySidekiq";
    args["card_type"] = part_str;
    args["card"]   = std::to_string(card);
    args["serial"]   = serial;
    args["rx_channels"]   = std::to_string(num_rx_channels);
    args["tx_channels"]   = std::to_string(num_tx_channels);

    return args;
}

/*******************************************************************
 * Channels API
 ******************************************************************/

size_t SoapySidekiq::getNumChannels(const int dir) const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "getNumChannels");

    if (dir == SOAPY_SDR_RX)
    {
        return num_rx_channels;
    }
    else if (dir == SOAPY_SDR_TX)
    {
        return num_tx_channels;
    }
    else
    {
        throw std::invalid_argument("invalid stream direction " + std::to_string(dir));
    }

}

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapySidekiq::listAntennas(const int direction, const size_t channel) const
{
    const auto name = currentAntenna(direction, channel);
    return name.empty() ? std::vector<std::string>{} : std::vector<std::string>{name};
}

std::string SoapySidekiq::currentAntenna(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    skiq_rf_port_t port = skiq_rf_port_unknown;
    const int status = direction == SOAPY_SDR_RX
                           ? skiq_read_rx_rf_port_for_hdl(card, getRxHandle(channel), &port)
                           : skiq_read_tx_rf_port_for_hdl(card, getTxHandle(channel), &port);
    // RF-port reporting is optional; do not advertise a fabricated choice.
    if (status == -ENOTSUP || status == -ENOSYS)
        return {};
    if (status != 0)
        throw std::runtime_error(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                                 " RF-port read failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", status " +
                                 std::to_string(status) + ")");
    if (port == skiq_rf_port_unknown)
        return {};
    const char *name = skiq_rf_port_string(port);
    if (name == nullptr || name[0] == '\0')
        throw std::runtime_error("SDK returned an invalid RF-port name (card " +
                                 std::to_string(card) + ", channel " + std::to_string(channel) +
                                 ")");
    return name;
}

std::string SoapySidekiq::getAntenna(const int direction, const size_t channel) const
{
    return currentAntenna(direction, channel);
}

void SoapySidekiq::setAntenna(const int direction, const size_t channel, const std::string &name)
{
    const auto current = currentAntenna(direction, channel);
    // The driver currently exposes only the configured route, so selecting it
    // is idempotent. Other selections must not silently succeed via the base API.
    if (current.empty() || name != current)
        throw std::invalid_argument("antenna selection '" + name + "' is unavailable (card " +
                                    std::to_string(card) + ", channel " + std::to_string(channel) +
                                    "); only the configured RF port can be selected");
}

/*******************************************************************
 * Frontend corrections API
 ******************************************************************/

bool SoapySidekiq::hasDCOffsetMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
        return false;
    const auto handle = getRxHandle(channel);
    uint32_t available{};
    const int status = skiq_read_rx_cal_types_avail(card, handle, &available);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_types_avail failed (card " +
                                 std::to_string(card) + ", channel " + std::to_string(channel) +
                                 ", handle " + std::to_string(handle) + ", status " +
                                 std::to_string(status) + ")");
    return (available & skiq_rx_cal_type_dc_offset) != 0;
}

void SoapySidekiq::setDCOffsetMode(const int direction, const size_t channel, const bool automatic)
{
    validateChannel(direction, channel);
    if (!hasDCOffsetMode(direction, channel))
    {
        if (automatic)
            throw std::invalid_argument("automatic DC-offset correction is unavailable (card " +
                                        std::to_string(card) + ", direction " +
                                        std::to_string(direction) + ", channel " +
                                        std::to_string(channel) + ")");
        return;
    }
    const auto handle = getRxHandle(channel);
    const auto context = " (card " + std::to_string(card) + ", channel " + std::to_string(channel) +
                         ", handle " + std::to_string(handle) + ")";
    const auto check = [&](const int status, const char *operation)
    {
        if (status != 0)
            throw std::runtime_error(std::string(operation) + context + " failed, status " +
                                     std::to_string(status));
    };
    uint32_t original_mask{};
    check(skiq_read_rx_cal_type_mask(card, handle, &original_mask), "skiq_read_rx_cal_type_mask");
    const uint32_t desired_mask =
        automatic ? original_mask | skiq_rx_cal_type_dc_offset
                  : original_mask & ~static_cast<uint32_t>(skiq_rx_cal_type_dc_offset);
    skiq_rx_cal_mode_t original_mode = skiq_rx_cal_mode_manual;
    if (automatic)
    {
        check(skiq_read_rx_cal_mode(card, handle, &original_mode), "skiq_read_rx_cal_mode");
        if (original_mode != skiq_rx_cal_mode_manual && original_mode != skiq_rx_cal_mode_auto)
            throw std::runtime_error("unsupported RX calibration mode" + context);
    }
    if (desired_mask != original_mask)
    {
        const int status = skiq_write_rx_cal_type_mask(card, handle, desired_mask);
        if (status != 0)
        {
            const int restore = skiq_write_rx_cal_type_mask(card, handle, original_mask);
            throw std::runtime_error("skiq_write_rx_cal_type_mask" + context + " failed, status " +
                                     std::to_string(status) + "; rollback mask status " +
                                     std::to_string(restore));
        }
    }

    // Disable only the DC bit: the shared calibration mode and other selected
    // algorithms belong to the rest of the RF configuration too.
    if (!automatic || original_mode == skiq_rx_cal_mode_auto)
        return;
    const int status = skiq_write_rx_cal_mode(card, handle, skiq_rx_cal_mode_auto);
    if (status != 0)
    {
        // Restore the snapshot after a failed second configuration step. SDK
        // errors cannot guarantee atomic hardware updates, so report failed
        // compensation as well as the original failure.
        const int mode_restore = skiq_write_rx_cal_mode(card, handle, original_mode);
        const int mask_restore = desired_mask == original_mask
                                     ? 0
                                     : skiq_write_rx_cal_type_mask(card, handle, original_mask);
        throw std::runtime_error("skiq_write_rx_cal_mode" + context + " failed, status " +
                                 std::to_string(status) + "; rollback mode status " +
                                 std::to_string(mode_restore) + ", mask status " +
                                 std::to_string(mask_restore));
    }
}

bool SoapySidekiq::getDCOffsetMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (!hasDCOffsetMode(direction, channel))
        return false;
    const auto handle = getRxHandle(channel);
    const auto context = " (card " + std::to_string(card) + ", channel " + std::to_string(channel) +
                         ", handle " + std::to_string(handle) + ")";
    uint32_t mask{};
    int status = skiq_read_rx_cal_type_mask(card, handle, &mask);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_type_mask" + context + " failed, status " +
                                 std::to_string(status));
    if ((mask & skiq_rx_cal_type_dc_offset) == 0)
        return false;
    skiq_rx_cal_mode_t mode = skiq_rx_cal_mode_manual;
    status = skiq_read_rx_cal_mode(card, handle, &mode);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_cal_mode" + context + " failed, status " +
                                 std::to_string(status));
    if (mode != skiq_rx_cal_mode_auto && mode != skiq_rx_cal_mode_manual)
        throw std::runtime_error("unsupported RX calibration mode" + context);
    return mode == skiq_rx_cal_mode_auto;
}

/*******************************************************************
 * Gain API
 ******************************************************************/

SoapySidekiq::GainProfile SoapySidekiq::gainProfile() const
{
    switch (part)
    {
    case skiq_mpcie:
    case skiq_m2:
    case skiq_m2_2280:
    case skiq_z2:
    case skiq_z3u:
        return {0, 1, 76};
    case skiq_x2:
    case skiq_x4:
    case skiq_x40:
        return {195, 2, 30};
    case skiq_nv100:
    case skiq_nvm2:
    case skiq_z4:
    case skiq_z4_mp:
        return {187, 2, 34};
    default:
        throw std::runtime_error("unsupported Sidekiq gain profile (card " + std::to_string(card) +
                                 ")");
    }
}

double SoapySidekiq::txGainMaximum(const size_t channel) const
{
    validateChannel(SOAPY_SDR_TX, channel);
    const auto maximum = param.tx_param[channel].atten_quarter_db_max;
    const auto minimum = param.tx_param[channel].atten_quarter_db_min;
    if (minimum > maximum)
        throw std::runtime_error("invalid TX attenuation limits (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ")");
    return static_cast<double>(maximum - minimum) / 4.0;
}

void SoapySidekiq::validateGainName(const int direction, const size_t channel,
                                    const std::string &name) const
{
    const auto choices = listGains(direction, channel);
    if (std::find(choices.begin(), choices.end(), name) == choices.end())
        throw std::invalid_argument(
            "unknown gain element '" + name + "' (card " + std::to_string(card) + ", direction " +
            std::to_string(direction) + ", channel " + std::to_string(channel) + ")");
}

std::vector<std::string> SoapySidekiq::listGains(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    // Retain the existing element name for client compatibility.
    return {"LNA"};
}

bool SoapySidekiq::hasGainMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    return direction == SOAPY_SDR_RX;
}

void SoapySidekiq::setGainMode(const int direction, const size_t channel, const bool automatic)
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
    {
        if (automatic)
            throw std::invalid_argument("TX automatic gain mode is not supported");
        // Manual-only TX already satisfies a request to disable AGC.
        return;
    }
    const auto handle = getRxHandle(channel);
    const auto mode = automatic ? skiq_rx_gain_auto : skiq_rx_gain_manual;
    const int status = skiq_write_rx_gain_mode(card, handle, mode);
    if (status != 0)
        throw std::runtime_error("skiq_write_rx_gain_mode failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", handle " +
                                 std::to_string(handle) + ", status " + std::to_string(status) +
                                 ")");
}

bool SoapySidekiq::getGainMode(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_TX)
        return false;
    const auto handle = getRxHandle(channel);
    skiq_rx_gain_t mode = skiq_rx_gain_manual;
    const int status = skiq_read_rx_gain_mode(card, handle, &mode);
    if (status != 0)
        throw std::runtime_error("skiq_read_rx_gain_mode failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", handle " +
                                 std::to_string(handle) + ", status " + std::to_string(status) +
                                 ")");
    if (mode != skiq_rx_gain_manual && mode != skiq_rx_gain_auto)
        throw std::runtime_error("skiq_read_rx_gain_mode returned an unsupported gain mode");
    return mode == skiq_rx_gain_auto;
}

void SoapySidekiq::setGain(const int direction,
                           const size_t channel,
                           const std::string &name,
                           const double value)
{
    validateGainName(direction, channel, name);
    setGain(direction, channel, value);
}

void SoapySidekiq::setGain(const int direction, const size_t channel, const double value)
{
    validateChannel(direction, channel);
    if (!std::isfinite(value))
        throw std::invalid_argument("setGain requires a finite dB value");
    const auto context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile();
        const auto handle = getRxHandle(channel);
        uint8_t minimum{}, maximum{};
        int status = skiq_read_rx_gain_index_range(card, handle, &minimum, &maximum);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain_index_range" + context +
                                     " failed, status " + std::to_string(status));
        if (minimum > maximum)
            throw std::runtime_error("invalid RX gain index range" + context);

        // Clamp in the dB domain before multiplication/rounding and narrowing.
        // This also keeps even DBL_MAX requests from overflowing intermediate math.
        const double bounded =
            std::clamp(value, (minimum - profile.rx_offset) / profile.rx_steps_per_db,
                       (maximum - profile.rx_offset) / profile.rx_steps_per_db);
        const double index = profile.rx_offset + std::round(bounded * profile.rx_steps_per_db);
        const auto gain_index = static_cast<uint8_t>(
            std::clamp(index, static_cast<double>(minimum), static_cast<double>(maximum)));

        // Validate profile/range before changing hardware gain mode.
        skiq_rx_gain_t mode = skiq_rx_gain_manual;
        status = skiq_read_rx_gain_mode(card, handle, &mode);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain_mode" + context + " failed, status " +
                                     std::to_string(status));
        if (mode == skiq_rx_gain_auto)
        {
            status = skiq_write_rx_gain_mode(card, handle, skiq_rx_gain_manual);
            if (status != 0)
                throw std::runtime_error("skiq_write_rx_gain_mode" + context + " failed, status " +
                                         std::to_string(status));
        }
        status = skiq_write_rx_gain(card, handle, gain_index);
        if (status != 0)
            throw std::runtime_error("skiq_write_rx_gain" + context + " failed, status " +
                                     std::to_string(status));
    }
    else
    {
        const auto maximum_gain = txGainMaximum(channel);
        if (value < 0 || value > maximum_gain)
            throw std::invalid_argument("TX gain" + context + " must be between 0 and " +
                                        std::to_string(maximum_gain) + " dB");
        const auto maximum = param.tx_param[channel].atten_quarter_db_max;
        const auto index = static_cast<uint16_t>(maximum - std::round(value * 4.0));
        const int status = skiq_write_tx_attenuation(card, getTxHandle(channel), index);
        if (status != 0)
            throw std::runtime_error("skiq_write_tx_attenuation" + context + " failed, status " +
                                     std::to_string(status));
    }
}

double SoapySidekiq::getGain(const int direction, const size_t channel, const std::string &name) const
{
    validateGainName(direction, channel, name);
    return getGain(direction, channel);
}

double SoapySidekiq::getGain(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    const auto context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile();
        uint8_t index{};
        const auto handle = getRxHandle(channel);
        uint8_t minimum{}, maximum{};
        int status = skiq_read_rx_gain_index_range(card, handle, &minimum, &maximum);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain_index_range" + context +
                                     " failed, status " + std::to_string(status));
        if (minimum > maximum)
            throw std::runtime_error("invalid RX gain index range" + context);
        status = skiq_read_rx_gain(card, handle, &index);
        if (status != 0)
            throw std::runtime_error("skiq_read_rx_gain" + context + " failed, status " +
                                     std::to_string(status));
        if (index < minimum || index > maximum)
            throw std::runtime_error("RX gain readback is outside its index range" + context);
        return (index - profile.rx_offset) / profile.rx_steps_per_db;
    }
    uint16_t index{};
    txGainMaximum(channel); // Validate metadata before hardware access/subtraction.
    const auto maximum = param.tx_param[channel].atten_quarter_db_max;
    const int status = skiq_read_tx_attenuation(card, getTxHandle(channel), &index);
    if (status != 0)
        throw std::runtime_error("skiq_read_tx_attenuation" + context + " failed, status " +
                                 std::to_string(status));
    if (index < param.tx_param[channel].atten_quarter_db_min || index > maximum)
        throw std::runtime_error("TX attenuation readback is outside its limits" + context);
    return static_cast<double>(maximum - index) / 4.0;
}

SoapySDR::Range SoapySidekiq::getGainRange(const int    direction,
                                           const size_t channel,
                                           const std::string & name) const
{
    validateGainName(direction, channel, name);
    SoapySDR_log(SOAPY_SDR_TRACE, "getGainRange with name");
    return getGainRange(direction, channel);
}

SoapySDR::Range SoapySidekiq::getGainRange(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    if (direction == SOAPY_SDR_RX)
    {
        const auto profile = gainProfile();
        return SoapySDR::Range(0, profile.rx_maximum, 1.0 / profile.rx_steps_per_db);
    }
    return SoapySDR::Range(0, txGainMaximum(channel), 0.25);
}

/*******************************************************************
 * Frequency API
 ******************************************************************/

void SoapySidekiq::setFrequency(const int direction, const size_t channel,
                  const double frequency, const SoapySDR::Kwargs &args)
{
    validateChannel(direction, channel);
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "setFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        skiq_rx_hdl_t rx_hdl = getRxHandle(channel);
        const uint64_t requested_frequency =
            soapy_sidekiq::checkedUnsigned<uint64_t>(frequency, "setFrequency RX");

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting rx center freq: %lu",
                      requested_frequency);

        status = skiq_write_rx_LO_freq(this->card, rx_hdl, requested_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_rx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, requested_frequency, status);
            std::ostringstream oss;
            oss << "Failed to set RX LO frequency on card " << unsigned(this->card)
                << ", channel " << channel
                << " to " << requested_frequency
                << " Hz (status " << status << ")";
            throw std::runtime_error(oss.str());
        }
    }
    else if (direction == SOAPY_SDR_TX)
    {
        skiq_tx_hdl_t tx_hdl = getTxHandle(channel);
        const uint64_t requested_frequency =
            soapy_sidekiq::checkedUnsigned<uint64_t>(frequency, "setFrequency TX");

        SoapySDR_logf(SOAPY_SDR_INFO, "Setting tx center freq: %lu",
                      requested_frequency);

        status = skiq_write_tx_LO_freq(this->card, tx_hdl, requested_frequency);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_write_tx_LO_freq failed, (card %u, frequency "
                          "%lu), status %d",
                          this->card, requested_frequency, status);
            std::ostringstream oss;
            oss << "Failed to set TX LO frequency on card " << unsigned(this->card)
                << ", channel " << channel
                << " to " << requested_frequency
                << " Hz (status " << status << ")";
            throw std::runtime_error(oss.str());
        }
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }
}

double SoapySidekiq::getFrequency(const int direction, const size_t channel) const
{
    validateChannel(direction, channel);
    int status = 0;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequency");

    if (direction == SOAPY_SDR_RX)
    {
        uint64_t freq{};
        double tuned_freq = std::numeric_limits<double>::quiet_NaN();
        skiq_rx_hdl_t rx_hdl = getRxHandle(channel);
        status = skiq_read_rx_LO_freq(card, rx_hdl, &freq, &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_LO_freq (card %u), status %d",
                          card, status);
            throw std::runtime_error("skiq_read_rx_LO_freq failed (card " + std::to_string(card) +
                                     ", channel " + std::to_string(channel) + ", handle " +
                                     std::to_string(rx_hdl) + ", status " + std::to_string(status) +
                                     ")");
        }
        if (!std::isfinite(tuned_freq) || tuned_freq < 0 || tuned_freq >= std::ldexp(1.0, 64))
            throw std::runtime_error("skiq_read_rx_LO_freq returned invalid tuned frequency");
        return tuned_freq;
    }
    else if (direction == SOAPY_SDR_TX)
    {
        uint64_t freq{};
        double tuned_freq = std::numeric_limits<double>::quiet_NaN();
        skiq_tx_hdl_t tx_hdl = getTxHandle(channel);
        status = skiq_read_tx_LO_freq(card, tx_hdl, &freq, &tuned_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_tx_LO_freq (card %u), status %d",
                          card, status);
            throw std::runtime_error("skiq_read_tx_LO_freq failed (card " + std::to_string(card) +
                                     ", channel " + std::to_string(channel) + ", handle " +
                                     std::to_string(tx_hdl) + ", status " + std::to_string(status) +
                                     ")");
        }
        if (!std::isfinite(tuned_freq) || tuned_freq < 0 || tuned_freq >= std::ldexp(1.0, 64))
            throw std::runtime_error("skiq_read_tx_LO_freq returned invalid tuned frequency");
        return tuned_freq;
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return 0;
}

SoapySDR::RangeList SoapySidekiq::getFrequencyRange(const int direction,
                                                    const size_t channel) const
{
    validateChannel(direction, channel);
    SoapySDR::RangeList results;

    SoapySDR_log(SOAPY_SDR_TRACE, "getFrequencyRange");

    uint64_t max{};
    uint64_t min{};
    int      status = 0;

    if (direction == SOAPY_SDR_RX)
    {
        status = skiq_read_rx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_LO_freq_range failed,(card %u), status %d",
                          card, status);
            throw std::runtime_error("skiq_read_rx_LO_freq_range failed (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", status " + std::to_string(status) + ")");
        }
        if (min > max)
            throw std::runtime_error("skiq_read_rx_LO_freq_range returned an inverted range");
        results.push_back(SoapySDR::Range(min, max));
    }
    else if (direction == SOAPY_SDR_TX)
    {
        status = skiq_read_tx_LO_freq_range(card, &max, &min);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "skiq_read_tx_LO_freq_range failed (card %u), status %d",
                card, status);
            throw std::runtime_error("skiq_read_tx_LO_freq_range failed (card " +
                                     std::to_string(card) + ", channel " + std::to_string(channel) +
                                     ", status " + std::to_string(status) + ")");
        }
        if (min > max)
            throw std::runtime_error("skiq_read_tx_LO_freq_range returned an inverted range");
        results.push_back(SoapySDR::Range(min, max));
    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "invalid direction %d", direction);
        throw std::runtime_error("");
    }

    return results;
}

/*******************************************************************
 * Sample Rate API
 ******************************************************************/

void SoapySidekiq::setSampleRate(const int direction, const size_t channel, const double rate)
{
    validateChannel(direction, channel);
    const auto requested = soapy_sidekiq::checkedUnsigned<uint32_t>(rate, "setSampleRate");
    if (requested == 0)
        throw std::invalid_argument("setSampleRate requires at least one sample per second");
    const auto previous = readRateAndBandwidth(direction, channel);
    // Preserve the current hardware bandwidth, clamped to the new sample rate.
    applyRateAndBandwidth(direction, channel, requested,
                          std::min(previous.actual_bandwidth, requested));
}

double SoapySidekiq::getSampleRate(const int direction, const size_t channel) const
{
    return readRateAndBandwidth(direction, channel).actual_rate;
}

SoapySDR::RangeList SoapySidekiq::getSampleRateRange(const int direction,
                                                     const size_t channel) const
{
    validateChannel(direction, channel);
    uint32_t minimum{}, maximum{};
    const std::string context =
        " (card " + std::to_string(card) + ", channel " + std::to_string(channel) + ")";
    int status = skiq_read_min_sample_rate(card, &minimum);
    if (status != 0)
        throw std::runtime_error("skiq_read_min_sample_rate" + context + " failed, status " +
                                 std::to_string(status));
    status = skiq_read_max_sample_rate(card, &maximum);
    if (status != 0)
        throw std::runtime_error("skiq_read_max_sample_rate" + context + " failed, status " +
                                 std::to_string(status));
    if (minimum == 0 || minimum > maximum)
        throw std::runtime_error("invalid SDK sample-rate bounds" + context);
    return {SoapySDR::Range(minimum, maximum)};
}

SoapySidekiq::RateAndBandwidth SoapySidekiq::readRateAndBandwidth(const int direction,
                                                                  const size_t channel) const
{
    validateChannel(direction, channel);
    uint32_t configured_rate{}, configured_bw{}, actual_bw{};
    double actual_rate = std::numeric_limits<double>::quiet_NaN();
    const auto operation = std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                           " rate/bandwidth readback (card " + std::to_string(card) + ", channel " +
                           std::to_string(channel) + ")";
    const int status =
        direction == SOAPY_SDR_RX
            ? skiq_read_rx_sample_rate_and_bandwidth(card, getRxHandle(channel), &configured_rate,
                                                     &actual_rate, &configured_bw, &actual_bw)
            : skiq_read_tx_sample_rate_and_bandwidth(card, getTxHandle(channel), &configured_rate,
                                                     &actual_rate, &configured_bw, &actual_bw);
    if (status != 0)
        throw std::runtime_error(operation + " failed, status " + std::to_string(status));
    if (configured_rate == 0 || !std::isfinite(actual_rate) || actual_rate < 1 ||
        actual_rate >= std::ldexp(1.0, 32))
        throw std::runtime_error(operation + " returned an invalid sample rate");
    // Zero may request automatic bandwidth selection, but it is not a usable
    // actual filter width. The zero initializer also detects omitted output.
    if (actual_bw == 0)
        throw std::runtime_error(operation + " returned missing or zero actual bandwidth");
    return {configured_rate, actual_rate, actual_bw};
}

void SoapySidekiq::applyRateAndBandwidth(const int direction, const size_t channel,
                                         const uint32_t rate, const uint32_t bandwidth)
{
    validateChannel(direction, channel);
    auto &cached_rate = (direction == SOAPY_SDR_RX ? rx_sample_rates : tx_sample_rates).at(channel);
    auto &cached_bw = (direction == SOAPY_SDR_RX ? rx_bandwidths : tx_bandwidths).at(channel);
    const int status =
        direction == SOAPY_SDR_RX
            ? skiq_write_rx_sample_rate_and_bandwidth(card, getRxHandle(channel), rate, bandwidth)
            : skiq_write_tx_sample_rate_and_bandwidth(card, getTxHandle(channel), rate, bandwidth);
    if (status != 0)
        throw std::runtime_error(std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                                 " rate/bandwidth write failed (card " + std::to_string(card) +
                                 ", channel " + std::to_string(channel) + ", rate " +
                                 std::to_string(rate) + ", bandwidth " + std::to_string(bandwidth) +
                                 ", status " + std::to_string(status) + ")");

    // The SDK may write some outputs before reporting failure. Keep every
    // output local and commit both cache entries only after validated success.
    // This preserves last-known cache state; it cannot undo a hardware write.
    const auto observed = readRateAndBandwidth(direction, channel);
    cached_rate = static_cast<uint32_t>(observed.actual_rate);
    cached_bw = observed.actual_bandwidth;
}

void SoapySidekiq::setBandwidth(const int direction, const size_t channel, const double bw)
{
    validateChannel(direction, channel);
    const auto requested = soapy_sidekiq::checkedUnsigned<uint32_t>(bw, "setBandwidth");
    // Read the hardware rate: RX/TX clocks can be coupled and a cached rate
    // may have become stale after a change on another handle/direction.
    const auto previous = readRateAndBandwidth(direction, channel);
    applyRateAndBandwidth(direction, channel, previous.configured_rate, requested);
}

/***********************************************************
 * Some open source call listSampleRates and want a list of available
 * sample rates.  For AD9361 cards, these have contiguous sample rates
 * available.
 * For other cards, there are profiles and there are only specific rates available
 * For now we will just give a list of min-max, with steps of 250k Hz.
 */
std::vector<double> SoapySidekiq::listSampleRates(const int direction, const size_t channel) const
{
    const auto bounds = getSampleRateRange(direction, channel).front();
    const auto minimum = static_cast<uint64_t>(bounds.minimum());
    const auto maximum = static_cast<uint64_t>(bounds.maximum());
    std::vector<double> rates;
    // Keep the legacy compatibility grid. A wider counter prevents uint32
    // wraparound at a large SDK maximum; this does not enumerate RF profiles.
    for (uint64_t rate = minimum; rate <= maximum; rate += 250000)
        rates.push_back(static_cast<double>(rate));
    return rates;
}

double SoapySidekiq::getBandwidth(const int direction, const size_t channel) const
{
    return readRateAndBandwidth(direction, channel).actual_bandwidth;
}

/*******************************************************************
 * Settings API
 ******************************************************************/

SoapySDR::ArgInfoList SoapySidekiq::getSettingInfo(void) const
{
    SoapySDR::ArgInfoList setArgs;
    SoapySDR_logf(SOAPY_SDR_TRACE, "getSettingInfo");

    SoapySDR::ArgInfo settingArg;

    settingArg.key         = "iq_swap";
    settingArg.value = iq_swap ? "true" : "false";
    settingArg.name        = "I/Q Swap";
    settingArg.description = "Set I then Q format";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "counter";
    settingArg.value = counter ? "true" : "false";
    settingArg.name        = "counter";
    settingArg.description = "FPGA creates RAMP samples";
    settingArg.type        = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(settingArg);

    settingArg.key         = "sys_clock_freq";
    settingArg.value = std::to_string(sys_freq);
    settingArg.name        = "sys_clock_freq";
    settingArg.description = "Read-only frequency of the system timestamp clock";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.options.clear();
    settingArg.key         = "full_scale";
    settingArg.value       = std::to_string(static_cast<uint32_t>(this->maxValue));
    settingArg.name        = "full_scale";
    settingArg.description = "Read-only device-specific full-scale integer sample value";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
    setArgs.push_back(settingArg);

    settingArg.key         = "timetype";
    settingArg.value = timetype;
    settingArg.name        = "timetype";
    settingArg.description = "The type of timestamp to return in readStream";
    settingArg.type        = SoapySDR::ArgInfo::STRING;
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
        int32_t status = skiq_read_rx_overload_state(this->card,
                                                     getFirstRxHandle(),
                                                     &overload_state);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_overload_state failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }
        return std::to_string(overload_state);
    }
    else if (equalsIgnoreCase(key, "cal_offset"))
    {
        double cal_offset = 0;
        int32_t status = skiq_read_rx_cal_offset(this->card,
                                                     getFirstRxHandle(),
                                                     &cal_offset);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_rx_cal_offset failed, card: %u status: %d",
                          this->card, status);
            throw std::runtime_error("");
        }
        return std::to_string(cal_offset);
    }
    else
    {
        throw std::invalid_argument("Unknown setting: " + key);
    }
    return "";
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
            SoapySDR_logf(SOAPY_SDR_INFO,
                          "using kernel 1pps source fallback value %d for card %u",
                          pps_source, card);
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_1pps_source failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
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
            SoapySDR_logf(SOAPY_SDR_WARNING, "invalid pps_source %d", pps_source);
            break;
    }

    return "NONE";
}

void check_1pps(uint8_t card, skiq_1pps_source_t pps_source)
{
    int32_t status = 0;
    uint32_t pulseCount = 0;
    uint32_t tryCount = 0;
    uint64_t lastTimestamp = 0;
    uint64_t rfTs = 0;
    uint64_t sysTs = 0;
    bool has_pps_source = false;
    useconds_t one_second_usecs = 1000000;

    SoapySDR_logf(SOAPY_SDR_TRACE, "check_1pps");

    if ((pps_source == skiq_1pps_source_external) || (pps_source == skiq_1pps_source_host))
    {
        do
        {
            status = skiq_read_last_1pps_timestamp(card, &rfTs, &sysTs);
            if (0 != status)
            {
                SoapySDR_logf(SOAPY_SDR_WARNING, "failed to get timestamp for card %" PRIu8
                        " (status = %" PRIi32 "); will try again\n", card, status);
            }
            else
            {
                if (sysTs != lastTimestamp)
                {
                    SoapySDR_logf(SOAPY_SDR_TRACE, "sysTs %lu, "
                                           "lastTimesamp %lu, trycount %lu, status %d",
                                           sysTs, lastTimestamp, tryCount, status);
                    pulseCount++;
                    lastTimestamp = sysTs;
                    SoapySDR_logf(SOAPY_SDR_DEBUG, "found 1pps pulse");
                }
            }
            tryCount++;

            if (tryCount > 1)
            {
                if (pulseCount == tryCount)
                {
                    has_pps_source = true;
                }
            }

            SoapySDR_logf(SOAPY_SDR_DEBUG, "");

            /* sleep for a second */
            usleep(one_second_usecs);

        } while ((!has_pps_source) && (tryCount <= 1));

        if (has_pps_source)
        {
            SoapySDR_logf(SOAPY_SDR_INFO, "Expected 1pps, found 1pps pulse");
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_ERROR, "Expected 1pps, but no 1pps pulse found");
            throw std::runtime_error("");
        }
    }
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
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid pps_source %s", source.c_str());
    }

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
            throw std::runtime_error("");
        }
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "1pps source set to %s", source.c_str());

    check_1pps(card, pps_source);

    return;
}


bool SoapySidekiq::hasHardwareTime(const std::string &what="") const
{
    SoapySDR_logf(SOAPY_SDR_TRACE, "hasHardwareTime");

    if (equalsIgnoreCase(what, "rx_rf_timestamp"))
    {
        return true;
    }
    else if (equalsIgnoreCase(what, "tx_rf_timestamp"))
    {
        return true;
    }
    else if (equalsIgnoreCase(what, "sys_timestamp"))
    {
        return true;
    }
    else if (what == "")
    {
        return true;
    }


    return false;
}

/* Return the specific timestamp in nanoseconds */
long long SoapySidekiq::getHardwareTime(const std::string &what="") const
{
    int status = 0;
    uint64_t timestamp = 0;

    SoapySDR_logf(SOAPY_SDR_TRACE, "getHardwareTime");

    if (equalsIgnoreCase(what, "rx_rf_timestamp"))
    {
        status = skiq_read_curr_rx_timestamp(card, getFirstRxHandle(), &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_rx_timestamp failed, (card %u), status %d",
                           card, status);
            throw std::runtime_error("");
        }

        // if the user has not set the sample rate then we
        // must warn them and return
        if (rx_sample_rates.empty() || rx_sample_rates.front() == 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "Cannot convert an rf timestamp to time without"
                          "configuring the sample rate first.  Passing unconverted timestamp");

            return timestamp;
        }

        return convert_timestamp_to_nanos(timestamp, rx_sample_rates.front());
    }
    else if (equalsIgnoreCase(what, "tx_rf_timestamp"))
    {
        status = skiq_read_curr_tx_timestamp(card, getFirstTxHandle(), &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_rx_timestamp failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }

        // if the user has not set the sample rate then we
        // must warn them and return
        if (tx_sample_rates.empty() || tx_sample_rates.front() == 0)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING, "Cannot convert an rf timestamp to time without"
                          "configuring the sample rate first.  Passing unconverted timestamp");

            return timestamp;
        }
        return convert_timestamp_to_nanos(timestamp, tx_sample_rates.front());
    }
    else if (equalsIgnoreCase(what, "sys_timestamp"))
    {
        uint64_t current_sys_freq = 0;
        status = skiq_read_sys_timestamp_freq(card, &current_sys_freq);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_sys_timestamp_freq failed (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }

        status = skiq_read_curr_sys_timestamp(card, &timestamp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "skiq_read_curr_sys_timestamp failed, (card %u), status %d",
                          card, status);
            throw std::runtime_error("");
        }
        return convert_timestamp_to_nanos(timestamp, current_sys_freq);

    }
    else
    {
        SoapySDR_logf(SOAPY_SDR_WARNING, "invalid 'what' parameter %s", what.c_str());
    }

    return 0;
}

void SoapySidekiq::setHardwareTime(const long long timeNs, const std::string &what="")
{
    const bool now = what.empty() || equalsIgnoreCase(what, "now");
    const bool on_pps = equalsIgnoreCase(what, "1pps");
    if (!now && !on_pps)
        throw std::invalid_argument("Unknown hardware-time update: " + what);
    if (timeNs < 0)
        throw std::invalid_argument("Hardware time cannot be negative");
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
        throw std::runtime_error(
            std::string(now ? "skiq_update_timestamps" : "skiq_write_timestamp_update_on_1pps") +
            " failed: " + std::to_string(status));
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
    const int status = skiq_write_ref_clock_select(card, selected);
    if (status != 0)
        throw std::runtime_error("skiq_write_ref_clock_select failed: " + std::to_string(status));
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
