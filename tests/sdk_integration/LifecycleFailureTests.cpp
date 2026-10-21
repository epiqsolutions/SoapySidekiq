/**
 * @file LifecycleFailureTests.cpp
 * @brief Verifies concrete stop-failure ownership, configuration gates, and retries.
 */
#include "FakeSdk.hpp"
#include "TestHarness.hpp"
#include "SoapySidekiq.hpp"
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
SoapySDR::Kwargs arguments()
{
    return {{"card", "5"}, {"tx_buffer_count", "2"}, {"tx_block_size", "252"}};
}

void failOperation(const char *operation)
{
    std::lock_guard<std::mutex> lock(scripted_sdk::mutex);
    scripted_sdk::script.fail_operation = operation;
}
} // namespace

TEST_CASE("concrete failed RX stop retains the RF gate while TX submissions continue")
{
    scripted_sdk::reset();
    soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
    auto device = std::make_unique<SoapySidekiq>(arguments());
    auto *rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0});
    auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    REQUIRE_EQ(device->activateStream(tx), 0);
    REQUIRE_EQ(device->activateStream(rx), 0);
    failOperation("skiq_stop_rx_streaming");
    REQUIRE_EQ(device->deactivateStream(rx), SOAPY_SDR_STREAM_ERROR);
    const auto failed = scripted_sdk::snapshot();
    REQUIRE(failed.rx_active && failed.tx_active);
    REQUIRE_EQ(failed.rx_stops, 1u);
    REQUIRE_THROWS_AS(device->closeStream(rx), std::runtime_error);
    // Close retries the retained hardware stop; another failure must keep the token/gate.
    const auto close_failed = scripted_sdk::snapshot();
    REQUIRE(close_failed.rx_active);
    REQUIRE_EQ(close_failed.rx_stops, 2u);
    REQUIRE_EQ(device->activateStream(rx), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_THROWS_AS(device->setSampleRate(SOAPY_SDR_TX, 0, 2000000), std::logic_error);
    REQUIRE_THROWS_AS(device->setBandwidth(SOAPY_SDR_RX, 0, 250000), std::logic_error);
    REQUIRE(scripted_sdk::snapshot().calls == close_failed.calls);
    REQUIRE_EQ(device->getStreamMTU(rx), std::size_t{2});

    const auto mtu = device->getStreamMTU(tx);
    std::vector<std::int16_t> samples(2 * mtu, 17);
    const void *buffers[]{samples.data()};
    int flags = 0;
    REQUIRE_EQ(device->writeStream(tx, buffers, mtu, flags, 0, 0), static_cast<int>(mtu));
    scripted_sdk::completeAll();
    failOperation("");
    REQUIRE_EQ(device->deactivateStream(rx), 0);
    REQUIRE_EQ(scripted_sdk::snapshot().rx_stops, 3u);
    device->setSampleRate(SOAPY_SDR_TX, 0, 2000000);
    REQUIRE_EQ(device->getSampleRate(SOAPY_SDR_RX, 0), 2000000.0);
    REQUIRE_EQ(device->activateStream(rx), 0);
    std::int16_t output[4]{};
    void *output_buffers[]{output};
    long long time = 0;
    REQUIRE_EQ(device->readStream(rx, output_buffers, 2, flags, time, 1000000), 2);
    REQUIRE_EQ(time, 1000000000LL);
    flags = 0;
    REQUIRE_EQ(device->writeStream(tx, buffers, mtu, flags, 0, 0), static_cast<int>(mtu));
    scripted_sdk::completeAll();
    const auto submitted = scripted_sdk::snapshot();
    REQUIRE_EQ(submitted.transmitted.size(), std::size_t{2});
    REQUIRE_EQ(submitted.transmitted[0][0], std::int16_t{17});
    REQUIRE_EQ(submitted.transmitted[1][0], std::int16_t{17});
    REQUIRE_EQ(device->deactivateStream(rx), 0);
    REQUIRE_EQ(device->deactivateStream(rx), 0);
    REQUIRE_EQ(device->deactivateStream(tx), 0);
    device->closeStream(rx);
    device->closeStream(tx);
    device.reset();
    const auto stopped = scripted_sdk::snapshot();
    REQUIRE_EQ(stopped.rx_stops, 4u);
    REQUIRE_EQ(stopped.tx_stops, 1u);
    REQUIRE_EQ(stopped.exit_calls, 1u);
    REQUIRE(stopped.pending.empty());
}

TEST_CASE("concrete failed TX stop retains accepted contexts and closes write admission")
{
    scripted_sdk::reset();
    soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
    auto device = std::make_unique<SoapySidekiq>(arguments());
    auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    REQUIRE_EQ(device->activateStream(tx), 0);
    const auto mtu = device->getStreamMTU(tx);
    std::vector<std::int16_t> samples(4 * mtu, 77);
    const void *buffers[]{samples.data()};
    int flags = 0;
    REQUIRE_EQ(device->writeStream(tx, buffers, 2 * mtu, flags, 0, 0), static_cast<int>(2 * mtu));
    failOperation("skiq_stop_tx_streaming");
    REQUIRE_EQ(device->deactivateStream(tx), SOAPY_SDR_STREAM_ERROR);
    const auto failed = scripted_sdk::snapshot();
    REQUIRE(failed.tx_active && failed.initialized && failed.callback != nullptr);
    REQUIRE_EQ(failed.tx_stops, 1u);
    REQUIRE_EQ(failed.pending.size(), std::size_t{2});
    REQUIRE_EQ(failed.pending.front().block->data[0], std::int16_t{77});
    REQUIRE_EQ(device->writeStream(tx, buffers, mtu, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_EQ(device->activateStream(tx), SOAPY_SDR_STREAM_ERROR);
    REQUIRE_THROWS_AS(device->closeStream(tx), std::runtime_error);
    REQUIRE_EQ(device->getStreamMTU(tx), mtu);
    REQUIRE(scripted_sdk::snapshot().calls == failed.calls);
    scripted_sdk::completeAll();
    REQUIRE_EQ(scripted_sdk::snapshot().completions, 2u);
    failOperation("");
    REQUIRE_EQ(device->deactivateStream(tx), 0);
    REQUIRE_EQ(device->deactivateStream(tx), 0);
    device->closeStream(tx);
    device.reset();
    const auto stopped = scripted_sdk::snapshot();
    REQUIRE_EQ(stopped.tx_stops, 2u);
    REQUIRE_EQ(stopped.exit_calls, 1u);
    REQUIRE(stopped.pending.empty());
    REQUIRE(!stopped.initialized && stopped.callback == nullptr);
}
