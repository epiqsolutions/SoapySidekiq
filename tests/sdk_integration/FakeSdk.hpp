/**
 * @file FakeSdk.hpp
 * @brief Declares scripted SDK state for concrete-driver integration tests.
 */
#pragma once

#include <sidekiq_api.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace scripted_sdk
{
inline constexpr std::uint8_t card_id = 5;
struct RfState
{
    std::uint32_t rate{1000000}, bandwidth{900000};
};
struct PendingSend
{
    skiq_tx_callback_t callback{};
    skiq_tx_block_t *block{};
    void *context{};
};
/** All mutable fields are protected by mutex; snapshots copy values under that lock. */
struct Script
{
    std::vector<std::string> calls;
    std::string fail_operation;
    bool initialized{}, rx_active{}, tx_active{}, rx_delivered{};
    unsigned init_calls{}, exit_calls{}, rx_stops{}, tx_stops{}, completions{};
    std::uint8_t selected_card{};
    std::uint8_t rx_channels{2}, tx_channels{2};
    bool topology_supported{true};
    std::uint8_t topology{};
    skiq_ref_clock_select_t clock_source{skiq_ref_clock_invalid};
    skiq_1pps_source_t time_source{skiq_1pps_source_unavailable};
    skiq_rx_hdl_t rx_handle{skiq_rx_hdl_end};
    skiq_tx_hdl_t tx_handle{skiq_tx_hdl_end};
    skiq_tx_callback_t callback{};
    std::uint16_t tx_block_samples{252};
    std::array<RfState, 8> rx_rf, tx_rf;
    alignas(
        skiq_rx_block_t) std::array<std::uint8_t, SKIQ_RX_HEADER_SIZE_IN_BYTES + 8> rx_storage{};
    std::vector<std::vector<std::int16_t>> transmitted;
    std::vector<PendingSend> pending;
    std::vector<int> send_results;
    bool tx_pps_entered{}, tx_pps_released{};
};
extern std::mutex mutex;
extern Script script;
void reset(const std::string &failure = {});
Script snapshot();
/** Deliver captured callbacks in reverse order, including after device destruction. */
void completeAll() noexcept;
/** Wait at most two seconds for the real worker to enter scripted PPS startup. */
bool waitForTxPps();
/** Release a held scripted PPS call, including assertion-failure cleanup. */
void releaseTxPps() noexcept;
} // namespace scripted_sdk
