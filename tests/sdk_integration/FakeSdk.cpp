/**
 * @file FakeSdk.cpp
 * @brief Implements a hardware-free SDK script using the real SDK function types.
 */
#include "FakeSdk.hpp"
#include "DriverTestHooks.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace scripted_sdk
{
std::mutex mutex;
Script script;
namespace
{
std::condition_variable tx_pps_changed;
}

bool waitForTxPps()
{
    std::unique_lock<std::mutex> lock(mutex);
    return tx_pps_changed.wait_for(lock, std::chrono::seconds(2),
                                   [] { return script.tx_pps_entered; });
}

void releaseTxPps() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        script.tx_pps_released = true;
    }
    tx_pps_changed.notify_all();
}

void driverTestPoint(const DriverTestPoint point) noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++(point == DriverTestPoint::writer_pps_wait ? script.pps_writer_waits : script.pps_joins);
    }
    tx_pps_changed.notify_all();
}

bool waitForDriverPoint(const DriverTestPoint point, const unsigned previous_count)
{
    std::unique_lock<std::mutex> lock(mutex);
    return tx_pps_changed.wait_for(lock, std::chrono::seconds(2),
                                   [&]
                                   {
                                       return (point == DriverTestPoint::writer_pps_wait
                                                   ? script.pps_writer_waits
                                                   : script.pps_joins) > previous_count;
                                   });
}

void reset(const std::string &failure)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (script.initialized || !script.pending.empty())
        throw std::logic_error("previous scripted SDK ownership was not released");
    script = {};
    script.fail_operation = failure;
}

Script snapshot()
{
    std::lock_guard<std::mutex> lock(mutex);
    return script;
}

void completeAll() noexcept
{
    for (;;)
    {
        PendingSend send;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (script.pending.empty())
                return;
            send = script.pending.back();
            script.pending.pop_back();
            ++script.completions;
        }
        send.callback(0, send.block, send.context);
    }
}

namespace
{
// Callers hold mutex; receive polling deliberately avoids growing the call log.
int record(const char *name)
{
    script.calls.emplace_back(name);
    return script.fail_operation == name ? -EIO : 0;
}

int readRf(const char *name, const bool tx, const unsigned handle, std::uint32_t *rate,
           double *actual_rate, std::uint32_t *bandwidth, std::uint32_t *actual_bandwidth)
{
    std::lock_guard<std::mutex> lock(mutex);
    const int status = record(name);
    if (status != 0)
        return status;
    const auto &state = (tx ? script.tx_rf : script.rx_rf).at(handle);
    *rate = state.rate;
    *actual_rate = state.rate;
    *bandwidth = *actual_bandwidth = state.bandwidth;
    return 0;
}

int writeRf(const char *name, const bool tx, const unsigned handle, const std::uint32_t rate,
            const std::uint32_t bandwidth)
{
    std::lock_guard<std::mutex> lock(mutex);
    const int status = record(name);
    if (status != 0)
        return status;
    // Model shared RX/TX clocks while preserving other handles' filter settings.
    for (auto *direction : {&script.rx_rf, &script.tx_rf})
        for (auto &state : *direction)
            state.rate = rate;
    (tx ? script.tx_rf : script.rx_rf).at(handle).bandwidth = bandwidth;
    return 0;
}
} // namespace
} // namespace scripted_sdk

using namespace scripted_sdk;

extern "C"
{
    void skiq_register_logging(void (*)(int32_t, const char *)) {}

    int32_t skiq_get_cards(skiq_xport_type_t, uint8_t *count, uint8_t *cards)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_get_cards");
        *count = 1;
        cards[0] = card_id;
        return status;
    }

    int32_t skiq_read_serial_string(uint8_t, char **serial)
    {
        static char value[] = "SCRIPTED-5";
        *serial = value;
        return 0;
    }

    int32_t skiq_is_card_avail(uint8_t, pid_t *owner)
    {
        *owner = 0;
        return 0;
    }

    int32_t skiq_init(skiq_xport_type_t, skiq_xport_init_level_t, uint8_t *cards, uint8_t count)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++script.init_calls;
        script.selected_card = cards[0];
        const int status = record("skiq_init");
        if (status != 0)
            return status;
        if (count != 1 || cards[0] != card_id || script.initialized)
            return -EINVAL;
        script.initialized = true;
        return 0;
    }

    int32_t skiq_exit(void)
    {
        std::lock_guard<std::mutex> lock(mutex);
        record("skiq_exit");
        ++script.exit_calls;
        script.initialized = false;
        script.callback = nullptr;
        return 0;
    }

    bool skiq_is_topology_supported(uint8_t card)
    {
        std::lock_guard<std::mutex> lock(mutex);
        record("skiq_is_topology_supported");
        return script.initialized && card == card_id && script.topology_supported;
    }

    int32_t skiq_apply_topology(uint8_t card, uint8_t topology)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_apply_topology");
        if (!script.initialized || card != card_id)
            return -EINVAL;
        if (status == 0)
            script.topology = topology;
        return status;
    }

    int32_t skiq_write_ref_clock_select(uint8_t card, skiq_ref_clock_select_t source)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_write_ref_clock_select");
        if (!script.initialized || card != card_id)
            return -EINVAL;
        if (status == 0)
            script.clock_source = source;
        return status;
    }

    int32_t skiq_write_1pps_source(uint8_t card, skiq_1pps_source_t source)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_write_1pps_source");
        if (!script.initialized || card != card_id)
            return -EINVAL;
        if (status == 0)
            script.time_source = source;
        return status;
    }

    int32_t skiq_read_last_1pps_timestamp(uint8_t card, uint64_t *rf, uint64_t *system)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_read_last_1pps_timestamp");
        if (!script.initialized || card != card_id)
            return -EINVAL;
        if (status == 0)
            *rf = *system = 0; // No physical edge is modeled.
        return status;
    }

    int32_t skiq_read_parameters(uint8_t, skiq_param_t *parameters)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_read_parameters");
        if (status != 0)
            return status;
        *parameters = {};
        parameters->card_param.part_type = skiq_nv100;
        parameters->rx_param[0].handle = skiq_rx_hdl_B1;
        parameters->rx_param[1].handle = skiq_rx_hdl_A1;
        parameters->tx_param[0].handle = skiq_tx_hdl_B1;
        parameters->tx_param[1].handle = skiq_tx_hdl_A1;
        return 0;
    }

    const char *skiq_part_string(skiq_part_t)
    {
        return "Scripted radio";
    }

    int32_t skiq_read_sys_timestamp_freq(uint8_t, uint64_t *frequency)
    {
        std::lock_guard<std::mutex> lock(mutex);
        *frequency = 1000000;
        return record("skiq_read_sys_timestamp_freq");
    }

    int32_t skiq_read_num_rx_chans(uint8_t, uint8_t *count)
    {
        std::lock_guard<std::mutex> lock(mutex);
        *count = script.rx_channels;
        return record("skiq_read_num_rx_chans");
    }

    int32_t skiq_read_num_tx_chans(uint8_t, uint8_t *count)
    {
        std::lock_guard<std::mutex> lock(mutex);
        *count = script.tx_channels;
        return record("skiq_read_num_tx_chans");
    }

    int32_t skiq_read_rx_iq_resolution(uint8_t, uint8_t *resolution)
    {
        std::lock_guard<std::mutex> lock(mutex);
        *resolution = 12;
        return record("skiq_read_rx_iq_resolution");
    }

    int32_t skiq_read_tx_iq_resolution(uint8_t, uint8_t *resolution)
    {
        std::lock_guard<std::mutex> lock(mutex);
        *resolution = 12;
        return record("skiq_read_tx_iq_resolution");
    }

    int32_t skiq_register_tx_complete_callback(uint8_t, skiq_tx_callback_t callback)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record(callback ? "skiq_register_tx_complete_callback" : "unregister");
        if (status == 0)
            script.callback = callback;
        return status;
    }

#define SCRIPT_SUCCESS(name, parameters)                                                           \
    int32_t name parameters                                                                        \
    {                                                                                              \
        std::lock_guard<std::mutex> lock(mutex);                                                   \
        return record(#name);                                                                      \
    }
    SCRIPT_SUCCESS(skiq_write_iq_order_mode, (uint8_t, skiq_iq_order_t))
    SCRIPT_SUCCESS(skiq_write_chan_mode, (uint8_t, skiq_chan_mode_t))
    SCRIPT_SUCCESS(skiq_set_rx_transfer_timeout, (const uint8_t, const int32_t))
    SCRIPT_SUCCESS(skiq_write_tx_data_flow_mode, (uint8_t, skiq_tx_hdl_t, skiq_tx_flow_mode_t))
    SCRIPT_SUCCESS(skiq_write_tx_transfer_mode, (uint8_t, skiq_tx_hdl_t, skiq_tx_transfer_mode_t))
    SCRIPT_SUCCESS(skiq_write_num_tx_threads, (uint8_t, uint8_t))
#undef SCRIPT_SUCCESS

    int32_t skiq_read_rx_sample_rate_and_bandwidth(uint8_t, skiq_rx_hdl_t handle, uint32_t *rate,
                                                   double *actual_rate, uint32_t *bandwidth,
                                                   uint32_t *actual_bandwidth)
    {
        return readRf("skiq_read_rx_sample_rate_and_bandwidth", false, handle, rate, actual_rate,
                      bandwidth, actual_bandwidth);
    }

    int32_t skiq_read_tx_sample_rate_and_bandwidth(uint8_t, skiq_tx_hdl_t handle, uint32_t *rate,
                                                   double *actual_rate, uint32_t *bandwidth,
                                                   uint32_t *actual_bandwidth)
    {
        return readRf("skiq_read_tx_sample_rate_and_bandwidth", true, handle, rate, actual_rate,
                      bandwidth, actual_bandwidth);
    }

    int32_t skiq_write_rx_sample_rate_and_bandwidth(uint8_t, skiq_rx_hdl_t handle, uint32_t rate,
                                                    uint32_t bandwidth)
    {
        return writeRf("skiq_write_rx_sample_rate_and_bandwidth", false, handle, rate, bandwidth);
    }

    int32_t skiq_write_tx_sample_rate_and_bandwidth(uint8_t, skiq_tx_hdl_t handle, uint32_t rate,
                                                    uint32_t bandwidth)
    {
        return writeRf("skiq_write_tx_sample_rate_and_bandwidth", true, handle, rate, bandwidth);
    }

    int32_t skiq_read_rx_block_size(uint8_t, skiq_rx_stream_mode_t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_read_rx_block_size");
        if (status != 0)
            return status;
        return static_cast<int32_t>(SKIQ_RX_HEADER_SIZE_IN_BYTES + 8);
    }

    int32_t skiq_write_tx_block_size(uint8_t, skiq_tx_hdl_t, uint16_t samples)
    {
        std::lock_guard<std::mutex> lock(mutex);
        script.tx_block_samples = samples;
        return record("skiq_write_tx_block_size");
    }

    int32_t skiq_start_rx_streaming(uint8_t, skiq_rx_hdl_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_start_rx_streaming");
        if (status != 0)
            return status;
        script.rx_active = true;
        script.rx_delivered = false;
        script.rx_handle = handle;
        return 0;
    }

    int32_t skiq_stop_rx_streaming(uint8_t, skiq_rx_hdl_t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++script.rx_stops;
        const int status = record("skiq_stop_rx_streaming");
        if (status == 0)
            script.rx_active = false;
        return status;
    }

    skiq_rx_status_t skiq_receive(uint8_t, skiq_rx_hdl_t *handle, skiq_rx_block_t **block,
                                  uint32_t *length)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!script.rx_active || script.rx_delivered)
            return skiq_rx_status_no_data;
        auto *packet = reinterpret_cast<skiq_rx_block_t *>(script.rx_storage.data());
        packet->rf_timestamp = 2000000;
        packet->sys_timestamp = 1000000;
        packet->data[0] = 1;
        packet->data[1] = -2;
        packet->data[2] = 3;
        packet->data[3] = -4;
        *handle = script.rx_handle;
        *block = packet;
        *length = static_cast<uint32_t>(script.rx_storage.size());
        script.rx_delivered = true;
        return skiq_rx_status_success;
    }

    int32_t skiq_start_tx_streaming(uint8_t, skiq_tx_hdl_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int status = record("skiq_start_tx_streaming");
        if (status == 0)
        {
            script.tx_active = true;
            script.tx_handle = handle;
        }
        return status;
    }

    int32_t skiq_start_tx_streaming_on_1pps(uint8_t card, skiq_tx_hdl_t handle, uint64_t timestamp)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const int status = record("skiq_start_tx_streaming_on_1pps");
        if (card != card_id || !script.initialized || timestamp != 0)
            return -EINVAL;
        script.tx_pps_entered = true;
        tx_pps_changed.notify_all();
        tx_pps_changed.wait(lock, [] { return script.tx_pps_released; });
        script.calls.emplace_back("TX PPS returned");
        if (status == 0)
        {
            script.tx_active = true;
            script.tx_handle = handle;
        }
        return status;
    }

    int32_t skiq_stop_tx_streaming_on_1pps(uint8_t card, skiq_tx_hdl_t, uint64_t timestamp)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++script.tx_stops;
        const int status = record("skiq_stop_tx_streaming_on_1pps");
        if (card != card_id || !script.initialized || timestamp != 0)
            return -EINVAL;
        if (!script.tx_active)
            return -ENODEV;
        if (status == 0)
            script.tx_active = false;
        return status;
    }

    int32_t skiq_stop_tx_streaming(uint8_t, skiq_tx_hdl_t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++script.tx_stops;
        const int status = record("skiq_stop_tx_streaming");
        if (status == 0)
            script.tx_active = false;
        return status;
    }

    int32_t skiq_transmit(uint8_t, skiq_tx_hdl_t, skiq_tx_block_t *block, void *context)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int forced_status = record("skiq_transmit");
        if (forced_status != 0)
            return forced_status;
        if (!script.tx_active || script.callback == nullptr)
            return -EINVAL;
        if (!script.send_results.empty())
        {
            const int status = script.send_results.front();
            script.send_results.erase(script.send_results.begin());
            if (status != 0)
                return status;
        }
        // SDK payload fields are packed; copy bytes without binding an array reference.
        std::vector<std::int16_t> payload(2 * script.tx_block_samples);
        std::memcpy(payload.data(), block->data, payload.size() * sizeof(std::int16_t));
        script.transmitted.push_back(std::move(payload));
        script.pending.push_back({script.callback, block, context});
        return 0;
    }
} // extern "C"
