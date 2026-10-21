/**
 * @file DeviceLifecycle.cpp
 * @brief Implements device lifecycle, identification, and SDK callbacks.
 */

#include "SoapySidekiq.hpp"
#include <SoapySidekiq/RxStreamSession.hpp>
#include <SoapySidekiq/DeviceOptions.hpp>
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySidekiq/SidekiqRxBackend.hpp>
#include <SoapySidekiq/SampleScale.hpp>
#include <SoapySidekiq/StringUtils.hpp>
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cinttypes>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <sidekiq_types.h>

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

// compares two strings and if equal range and equal values per character
// returns true.
bool equalsIgnoreCase(const std::string &a, const std::string &b)
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

void SoapySidekiq::configureWhileRxInactive(const std::function<void()> &configure)
{
    if (rx_session != nullptr)
        rx_session->configureInactive(configure);
    else
        configure();
}

// Acquire constructor callback resources as one transaction. No stream can be
// active yet, so failed registration can be unwound before SDK initialization
// is released by the constructor's outer rollback guard.
void SoapySidekiq::initializeTxCallbacks()
{
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
}

// Constructor
SoapySidekiq::SoapySidekiq(const SoapySDR::Kwargs &args)
{
    int status = 0;
    uint8_t channels = 0;
    skiq_iq_order_t iq_order;
    // Reject all malformed constructor input before registering or opening SDK state.
    const auto options = soapy_sidekiq::parseDeviceOptions(args);
    tx_buffer_count = options.tx_buffer_count;
    tx_completion = std::make_shared<TxCompletion>(tx_buffer_count);

    /* Register our own logging function before initializing the library */
    skiq_register_logging(logging_handler);

    SoapySDR_logf(SOAPY_SDR_TRACE, "in constructor", card);

    /* We need to set some default parameters in case the user does not */
    rxUseShort = true;
    txUseShort = true;
    iq_swap = true;
    counter = false;
    card = 0;
    rx_payload_size_in_words = 1018;
    rfTimeSource = true;
    timetype = "rf_timestamp";

    card = options.card;
    current_tx_block_size = options.tx_block_size;
    SoapySDR_logf(SOAPY_SDR_INFO, "TX block size set to %u", current_tx_block_size);

    skiq_xport_type_t type = skiq_xport_type_auto;
    skiq_xport_init_level_t level = skiq_xport_init_level_full;

    SoapySDR_logf(SOAPY_SDR_INFO, "Sidekiq opening card %u", card);

    /* init sidekiq */
    status = skiq_init(type, level, &card, 1);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_init failed (card %u), status %d", card, status);
        throw std::runtime_error("skiq_init failed (card " + std::to_string(card) + ", status " +
                                 std::to_string(status) + ")");
    }

    // A throwing constructor never runs the device destructor. Only arm this
    // rollback after this call successfully acquires SDK initialization.
    soapy_sidekiq::ScopeExit rollback(
        [this]() noexcept
        {
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
                throw std::runtime_error("skiq_apply_topology failed (card " +
                                         std::to_string(card) + ", topology " +
                                         std::to_string(static_cast<int>(*options.topology)) +
                                         ", status " + std::to_string(status) + ")");
            }
        }
        else
        {
            throw std::invalid_argument("Topology " + std::to_string(*options.topology) +
                                        " is not supported by card " + std::to_string(card));
        }
    }

    status = skiq_read_parameters(card, &this->param);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_parameters failed, card %u, status %d", card,
                      status);
        throw std::runtime_error("skiq_read_parameters failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }

    status = skiq_read_sys_timestamp_freq(card, &sys_freq);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_sys_timestamp_freq failed (card %u), status %d",
                      card, status);
        throw std::runtime_error("skiq_read_sys_timestamp_freq failed (card " +
                                 std::to_string(card) + ", status " + std::to_string(status) + ")");
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
        throw std::runtime_error("skiq_write_iq_order_mode failed (card " + std::to_string(card) +
                                 ", IQ order " + std::to_string(static_cast<int>(iq_order)) +
                                 ", status " + std::to_string(status) + ")");
    }

    status = skiq_read_num_rx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_rx_chans failed, (card %u), status %d", card,
                      status);
        throw std::runtime_error("skiq_read_num_rx_chans failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    if (channels > std::size(param.rx_param))
        throw std::runtime_error("skiq_read_num_rx_chans returned too many channels (card " +
                                 std::to_string(card) + ", channels " + std::to_string(channels) +
                                 ", table capacity " + std::to_string(std::size(param.rx_param)) +
                                 ")");
    num_rx_channels = channels;

    for (size_t chan = 0; chan < num_rx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy RX channel %zu maps to Sidekiq rx handle %u", chan,
                      getRxHandle(chan));
    }

    status = skiq_read_num_tx_chans(card, &channels);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_num_tx_chans failed, (card %u), status %d", card,
                      status);
        throw std::runtime_error("skiq_read_num_tx_chans failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    if (channels > std::size(param.tx_param))
        throw std::runtime_error("skiq_read_num_tx_chans returned too many channels (card " +
                                 std::to_string(card) + ", channels " + std::to_string(channels) +
                                 ", table capacity " + std::to_string(std::size(param.tx_param)) +
                                 ")");
    num_tx_channels = channels;
    for (size_t chan = 0; chan < num_tx_channels; chan++)
    {
        SoapySDR_logf(SOAPY_SDR_INFO, "Soapy TX channel %zu maps to Sidekiq tx handle %u", chan,
                      getTxHandle(chan));
    }

    uint8_t tmp_resolution = 0;

    /* Every card can have a different iq resolution.  */
    status = skiq_read_rx_iq_resolution(card, &tmp_resolution);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR,
                      "skiq_read_rx_iq_resolution failed, "
                      "card: %u status: %d",
                      card, status);
        throw std::runtime_error("skiq_read_rx_iq_resolution failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
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
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_read_tx_iq_resolution failed, card: %u status: %d",
                      card, status);
        throw std::runtime_error("skiq_read_tx_iq_resolution failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    }
    // The SDK manual lists these products as upper-14-bit TX. Their low two
    // host bits are discarded, so DAC resolution is not the host amplitude width.
    const auto tx_alignment = (part == skiq_x2 || part == skiq_x4 || part == skiq_x40)
                                  ? soapy_sidekiq::TxSampleAlignment::upper_bits
                                  : soapy_sidekiq::TxSampleAlignment::right_justified;
    this->txMaxValue = soapy_sidekiq::txSampleFullScale(tx_resolution, tx_alignment);
    SoapySDR_logf(SOAPY_SDR_INFO, "card: %u, TX DAC resolution: %u bits, host full scale: %u", card,
                  tx_resolution, static_cast<uint32_t>(this->txMaxValue));

    status = skiq_write_chan_mode(card, skiq_chan_mode_single);
    if (status != 0)
    {
        SoapySDR_logf(SOAPY_SDR_ERROR, "skiq_write_chan_mode failed, card %u, status %d", card,
                      status);
        throw std::runtime_error("skiq_write_chan_mode failed (card " + std::to_string(card) +
                                 ", channel mode single" + ", status " + std::to_string(status) +
                                 ")");
    }
    SoapySDR_logf(SOAPY_SDR_TRACE, "channel mode set to single");

    // Apply the same checked SDK readback contract used by RF configuration.
    initializeRateCaches();

    // Source setters require initialized hardware, so apply validated names last.
    if (options.clock_source.has_value())
    {
        setClockSource(*options.clock_source);
    }

    if (options.time_source.has_value())
    {
        setTimeSource(*options.time_source);
    }

    initializeTxCallbacks();
    rollback.release();
}

// Destructor
SoapySidekiq::~SoapySidekiq(void)
{
    // No throwing diagnostics may precede shutdown in a noexcept destructor.
    shutdownTx();

    // Stop and join RX before invalidating callbacks or exiting the SDK.
    if (rx_session != nullptr)
    {
        rx_session->shutdown();
    }
    rx_session.reset();
    rx_backend.reset();
    active_rx_stream.reset();

    skiq_register_tx_complete_callback(card, nullptr);

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
    args["card"] = std::to_string(card);
    char *serial_string = nullptr;
    const int status = skiq_read_serial_string(card, &serial_string);
    if (status != 0)
        throw std::runtime_error("skiq_read_serial_string failed (card " + std::to_string(card) +
                                 ", status " + std::to_string(status) + ")");
    if (serial_string == nullptr || serial_string[0] == '\0')
        throw std::runtime_error("skiq_read_serial_string returned a missing serial (card " +
                                 std::to_string(card) + ")");
    // The SDK owns this buffer. Copy its value; never release the pointer.
    args["serial"] = serial_string;
    args["rx_channels"] = std::to_string(num_rx_channels);
    args["tx_channels"] = std::to_string(num_tx_channels);

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
