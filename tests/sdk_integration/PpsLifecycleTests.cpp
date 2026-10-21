/**
 * @file PpsLifecycleTests.cpp
 * @brief Exercises the concrete device while its real PPS worker waits in the SDK.
 */
#include "FakeSdk.hpp"
#include "TestHarness.hpp"
#include "SoapySidekiq.hpp"
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{
SoapySDR::Kwargs arguments()
{
    return {{"card", "5"}, {"tx_buffer_count", "2"}, {"tx_block_size", "252"}};
}

std::size_t position(const scripted_sdk::Script &state, const std::string &name)
{
    const auto found = std::find(state.calls.begin(), state.calls.end(), name);
    REQUIRE(found != state.calls.end());
    return static_cast<std::size_t>(std::distance(state.calls.begin(), found));
}
} // namespace

TEST_CASE("concrete pending PPS startup permits RX and joins before PPS TX stop")
{
    for (const bool startup_error : {false, true})
    {
        scripted_sdk::reset(startup_error ? "skiq_start_tx_streaming_on_1pps" : "");
        soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
        auto device = std::make_unique<SoapySidekiq>(arguments());
        soapy_sidekiq::ScopeExit release_before_device([]() noexcept
                                                       { scripted_sdk::releaseTxPps(); });
        auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
        auto *rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0});
        REQUIRE_EQ(device->activateStream(tx, SOAPY_SDR_HAS_TIME, 0), 0);
        REQUIRE(scripted_sdk::waitForTxPps());
        REQUIRE_EQ(device->activateStream(rx), 0);
        std::int16_t output[4]{};
        void *buffers[]{output};
        int flags = 0;
        long long time = 0;
        REQUIRE_EQ(device->readStream(rx, buffers, 2, flags, time, 1000000), 2);
        REQUIRE_EQ(output[0], std::int16_t{1});
        REQUIRE_EQ(time, 2000000000LL);
        const auto mtu = device->getStreamMTU(tx);
        std::vector<std::int16_t> samples(2 * mtu, 37);
        const void *input[]{samples.data()};
        flags = 0;
        REQUIRE_EQ(device->writeStream(tx, input, mtu, flags, 0, 0), SOAPY_SDR_TIMEOUT);
        REQUIRE(scripted_sdk::snapshot().pending.empty());
        REQUIRE_EQ(device->deactivateStream(rx), 0);

        auto writer =
            std::async(std::launch::async,
                       [&] { return device->writeStream(tx, input, mtu, flags, 0, 2000000); });
        soapy_sidekiq::ScopeExit release_before_writer_join([]() noexcept
                                                            { scripted_sdk::releaseTxPps(); });
        REQUIRE(writer.wait_for(20ms) == std::future_status::timeout);
        auto stopped = std::async(std::launch::async, [&]
                                  { return device->deactivateStream(tx, SOAPY_SDR_HAS_TIME, 0); });
        // Release before the future's destructor joins, even if an assertion fails.
        soapy_sidekiq::ScopeExit release_before_join([]() noexcept
                                                     { scripted_sdk::releaseTxPps(); });
        REQUIRE(stopped.wait_for(20ms) == std::future_status::timeout);
        REQUIRE_EQ(scripted_sdk::snapshot().tx_stops, 0u);
        // Cancelling the admitted writer must not wait for SDK startup to return.
        REQUIRE(writer.wait_for(1s) == std::future_status::ready);
        REQUIRE_EQ(writer.get(), SOAPY_SDR_STREAM_ERROR);
        scripted_sdk::releaseTxPps();
        REQUIRE_EQ(stopped.get(), 0);
        REQUIRE_EQ(device->writeStream(tx, input, mtu, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
        device->closeStream(tx);
        device->closeStream(rx);
        device.reset();
        const auto state = scripted_sdk::snapshot();
        REQUIRE(position(state, "TX PPS returned") <
                position(state, "skiq_stop_tx_streaming_on_1pps"));
        REQUIRE_EQ(state.tx_stops, 1u);
        REQUIRE_EQ(state.rx_stops, 1u);
        REQUIRE_EQ(state.exit_calls, 1u);
        REQUIRE(!state.initialized && !state.tx_active && !state.rx_active);
    }
}

TEST_CASE("concrete destructor joins held PPS startup before unregister and SDK exit")
{
    scripted_sdk::reset();
    auto device = std::make_unique<SoapySidekiq>(arguments());
    soapy_sidekiq::ScopeExit release_before_device([]() noexcept { scripted_sdk::releaseTxPps(); });
    auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    REQUIRE_EQ(device->activateStream(tx, SOAPY_SDR_HAS_TIME, 0), 0);
    REQUIRE(scripted_sdk::waitForTxPps());
    auto destroyed = std::async(std::launch::async, [&] { device.reset(); });
    soapy_sidekiq::ScopeExit release_before_join([]() noexcept { scripted_sdk::releaseTxPps(); });
    REQUIRE(destroyed.wait_for(20ms) == std::future_status::timeout);
    REQUIRE_EQ(scripted_sdk::snapshot().tx_stops, 0u);
    REQUIRE_EQ(scripted_sdk::snapshot().exit_calls, 0u);
    scripted_sdk::releaseTxPps();
    destroyed.get();
    const auto state = scripted_sdk::snapshot();
    REQUIRE(position(state, "TX PPS returned") < position(state, "skiq_stop_tx_streaming"));
    REQUIRE(position(state, "skiq_stop_tx_streaming") < position(state, "unregister"));
    REQUIRE(position(state, "unregister") < position(state, "skiq_exit"));
    REQUIRE_EQ(state.tx_stops, 1u);
    REQUIRE_EQ(state.exit_calls, 1u);
    REQUIRE(state.pending.empty());
    REQUIRE(!state.initialized && !state.tx_active);
}
