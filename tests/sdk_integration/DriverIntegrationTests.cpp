/**
 * @file DriverIntegrationTests.cpp
 * @brief Exercises real discovery, construction, streams, callbacks, and teardown.
 */
#include "FakeSdk.hpp"
#include "TestHarness.hpp"
#include <SoapySidekiq/ScopeExit.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <algorithm>
#include <cmath>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

// Compile the actual registry adapter in this TU so discovery/factory calls do
// not invoke an external plugin loader. The concrete class and every other
// implementation unit are compiled normally, without an alternate device class.
#include "../../Registration.cpp"

namespace
{
SoapySDR::Kwargs arguments()
{
    return {{"card", "5"}, {"tx_buffer_count", "2"}, {"tx_block_size", "252"}};
}

std::size_t callPosition(const scripted_sdk::Script &state, const std::string &name)
{
    const auto found = std::find(state.calls.begin(), state.calls.end(), name);
    REQUIRE(found != state.calls.end());
    return static_cast<std::size_t>(std::distance(state.calls.begin(), found));
}
} // namespace

TEST_CASE("concrete driver discovers constructs transfers both formats and shuts down")
{
    for (const auto &format : {std::string(SOAPY_SDR_CS16), std::string(SOAPY_SDR_CF32)})
    {
        scripted_sdk::reset();
        soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
        const auto discovered = findSidekiq({{"serial", "SCRIPTED-5"}});
        REQUIRE_EQ(discovered.size(), std::size_t{1});
        REQUIRE_EQ(discovered.front().at("card"), std::string("5"));
        std::unique_ptr<SoapySDR::Device> device(makeSidekiq(arguments()));
        REQUIRE_EQ(device->getDriverKey(), std::string("Sidekiq"));
        REQUIRE_EQ(device->getHardwareKey(), std::string("Scripted radio"));
        REQUIRE_EQ(device->getNumChannels(SOAPY_SDR_RX), std::size_t{2});
        device->setSampleRate(SOAPY_SDR_RX, 0, 2000000);
        device->setBandwidth(SOAPY_SDR_TX, 0, 250000);
        REQUIRE_EQ(device->getSampleRate(SOAPY_SDR_RX, 0), 2000000.0);
        REQUIRE_EQ(device->getSampleRate(SOAPY_SDR_TX, 0), 2000000.0);

        for (const auto &source : device->listClockSources())
        {
            auto args = arguments();
            args["clock_source"] = source;
            REQUIRE_EQ(soapy_sidekiq::parseDeviceOptions(args).clock_source.value(), source);
        }
        for (const auto &source : device->listTimeSources())
        {
            auto args = arguments();
            args["time_source"] = source;
            REQUIRE_EQ(soapy_sidekiq::parseDeviceOptions(args).time_source.value(), source);
        }

        const auto calls_before_invalid = scripted_sdk::snapshot().calls.size();
        REQUIRE_THROWS_AS(device->setupStream(SOAPY_SDR_RX, "invalid", {0}), std::invalid_argument);
        REQUIRE_EQ(scripted_sdk::snapshot().calls.size(), calls_before_invalid);
        auto *rx = device->setupStream(SOAPY_SDR_RX, format, {0});
        auto *tx = device->setupStream(SOAPY_SDR_TX, format, {0});
        REQUIRE_EQ(device->getStreamMTU(rx), std::size_t{2});
        const auto mtu = device->getStreamMTU(tx);
        REQUIRE_EQ(mtu, std::size_t{252});
        std::vector<std::int16_t> integers(4 * mtu);
        std::vector<float> floats(4 * mtu);
        for (std::size_t i = 0; i < 2 * mtu; ++i)
        {
            integers[2 * i] = 11;
            integers[2 * i + 1] = -22;
            floats[2 * i] = 1;
            floats[2 * i + 1] = -1;
        }
        const void *input[]{format == SOAPY_SDR_CS16 ? static_cast<const void *>(integers.data())
                                                     : static_cast<const void *>(floats.data())};
        int flags = 0;
        REQUIRE_EQ(device->writeStream(tx, input, mtu, flags, 0, 0), SOAPY_SDR_STREAM_ERROR);
        REQUIRE_EQ(device->activateStream(tx), 0);
        REQUIRE_EQ(device->activateStream(rx), 0);
        std::int16_t integer_output[4]{};
        float float_output[4]{};
        void *output[]{format == SOAPY_SDR_CS16 ? static_cast<void *>(integer_output)
                                                : static_cast<void *>(float_output)};
        long long time = 0;
        REQUIRE_EQ(device->readStream(rx, output, 2, flags, time, 1000000), 2);
        REQUIRE((flags & SOAPY_SDR_HAS_TIME) != 0);
        REQUIRE_EQ(time, 1000000000LL);
        if (format == SOAPY_SDR_CS16)
        {
            REQUIRE_EQ(integer_output[0], std::int16_t{1});
            REQUIRE_EQ(integer_output[3], std::int16_t{-4});
        }
        else
        {
            REQUIRE(std::abs(float_output[0] - 1.0F / 2047.0F) < 0.000001F);
            REQUIRE(std::abs(float_output[3] + 4.0F / 2047.0F) < 0.000001F);
        }
        flags = 0;
        REQUIRE_EQ(device->writeStream(tx, input, 2 * mtu, flags, 0, 1000000),
                   static_cast<int>(2 * mtu));
        const auto sent = scripted_sdk::snapshot();
        REQUIRE(sent.rx_handle == skiq_rx_hdl_B1);
        REQUIRE(sent.tx_handle == skiq_tx_hdl_B1);
        REQUIRE_EQ(sent.pending.size(), std::size_t{2});
        REQUIRE_EQ(sent.transmitted.size(), std::size_t{2});
        REQUIRE_EQ(sent.transmitted[0][0], format == SOAPY_SDR_CS16 ? 11 : 2047);
        REQUIRE_EQ(sent.transmitted[1][1], format == SOAPY_SDR_CS16 ? -22 : -2047);
        scripted_sdk::completeAll(); // Real callbacks, deliberately out of order.
        REQUIRE_EQ(scripted_sdk::snapshot().completions, 2u);
        REQUIRE_EQ(device->deactivateStream(rx), 0);
        REQUIRE_EQ(device->deactivateStream(rx), 0);
        device->closeStream(rx);
        REQUIRE_EQ(device->deactivateStream(tx), 0);
        REQUIRE_EQ(device->deactivateStream(tx), 0);
        device->closeStream(tx);
        device.reset();
        const auto stopped = scripted_sdk::snapshot();
        REQUIRE(!stopped.initialized && !stopped.rx_active && !stopped.tx_active);
        REQUIRE(stopped.pending.empty());
        REQUIRE_EQ(stopped.init_calls, 1u);
        REQUIRE_EQ(stopped.exit_calls, 1u);
        REQUIRE_EQ(stopped.rx_stops, 1u);
        REQUIRE_EQ(stopped.tx_stops, 1u);
        REQUIRE_EQ(stopped.selected_card, scripted_sdk::card_id);
        REQUIRE(callPosition(stopped, "skiq_stop_tx_streaming") <
                callPosition(stopped, "unregister"));
        REQUIRE(callPosition(stopped, "unregister") < callPosition(stopped, "skiq_exit"));
    }
}

TEST_CASE("concrete constructor rejects input and releases SDK acquisition on later errors")
{
    scripted_sdk::reset();
    REQUIRE_THROWS_AS(makeSidekiq({{"card", "invalid"}}), std::invalid_argument);
    REQUIRE(scripted_sdk::snapshot().calls.empty());
    for (const auto *operation :
         {"skiq_init", "skiq_read_parameters", "skiq_read_sys_timestamp_freq",
          "skiq_write_iq_order_mode", "skiq_read_num_rx_chans", "skiq_read_num_tx_chans",
          "skiq_read_rx_iq_resolution", "skiq_read_tx_iq_resolution", "skiq_write_chan_mode",
          "skiq_read_rx_sample_rate_and_bandwidth", "skiq_read_tx_sample_rate_and_bandwidth",
          "skiq_register_tx_complete_callback"})
    {
        scripted_sdk::reset(operation);
        REQUIRE_THROWS_AS(makeSidekiq(arguments()), std::runtime_error);
        const auto state = scripted_sdk::snapshot();
        REQUIRE(!state.initialized);
        REQUIRE(state.pending.empty());
        REQUIRE(state.callback == nullptr);
        REQUIRE_EQ(state.init_calls, 1u);
        REQUIRE_EQ(state.exit_calls, std::string(operation) == "skiq_init" ? 0u : 1u);
    }
}

TEST_CASE("invalid constructor sources never initialize the SDK")
{
    for (const auto *key : {"clock_source", "time_source"})
        for (const auto *value :
             {"unknown", "internal", " external_clock", "1pps_source_external "})
        {
            scripted_sdk::reset();
            auto args = arguments();
            args[key] = value;
            REQUIRE_THROWS_AS(makeSidekiq(args), std::invalid_argument);
            const auto state = scripted_sdk::snapshot();
            REQUIRE(state.calls.empty());
            REQUIRE_EQ(state.init_calls, 0u);
            REQUIRE_EQ(state.exit_calls, 0u);
        }
}

TEST_CASE("concrete TX backpressure and destruction retain an accepted context for delivery")
{
    scripted_sdk::reset();
    soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
    std::unique_ptr<SoapySDR::Device> device(makeSidekiq(arguments()));
    auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
    REQUIRE_EQ(device->activateStream(tx), 0);
    const auto mtu = device->getStreamMTU(tx);
    std::vector<std::int16_t> input(4 * mtu, 37);
    const void *buffers[]{input.data()};
    {
        std::lock_guard<std::mutex> lock(scripted_sdk::mutex);
        scripted_sdk::script.send_results = {0, SKIQ_TX_ASYNC_SEND_QUEUE_FULL};
    }
    int flags = 0;
    REQUIRE_EQ(device->writeStream(tx, buffers, 2 * mtu, flags, 0, 0), static_cast<int>(mtu));
    REQUIRE_THROWS_AS(device->closeStream(tx), std::runtime_error);
    device.reset(); // Actual destructor stops TX, drops its pool, unregisters, exits.
    const auto state = scripted_sdk::snapshot();
    REQUIRE_EQ(state.exit_calls, 1u);
    REQUIRE_EQ(state.tx_stops, 1u);
    REQUIRE_EQ(state.pending.size(), std::size_t{1});
    REQUIRE_EQ(state.pending[0].block->data[0], std::int16_t{37});
    scripted_sdk::completeAll();
    REQUIRE(scripted_sdk::snapshot().pending.empty());
    REQUIRE_EQ(scripted_sdk::snapshot().completions, 1u);
}

TEST_CASE("concrete constructor applies optional sources before publishing callbacks")
{
    scripted_sdk::reset();
    auto args = arguments();
    args["topology"] = "17";
    args["clock_source"] = "External_Clock";
    args["time_source"] = "1PPS_SOURCE_UNAVAILABLE";
    {
        std::unique_ptr<SoapySDR::Device> device(makeSidekiq(args));
        const auto state = scripted_sdk::snapshot();
        REQUIRE(state.initialized && state.callback != nullptr);
        REQUIRE_EQ(state.topology, std::uint8_t{17});
        REQUIRE(state.clock_source == skiq_ref_clock_external);
        REQUIRE(state.time_source == skiq_1pps_source_unavailable);
        REQUIRE_EQ(state.init_calls, 1u);
        REQUIRE_EQ(state.exit_calls, 0u);
        REQUIRE(callPosition(state, "skiq_init") <
                callPosition(state, "skiq_is_topology_supported"));
        REQUIRE(callPosition(state, "skiq_apply_topology") <
                callPosition(state, "skiq_read_parameters"));
        REQUIRE(callPosition(state, "skiq_write_ref_clock_select") <
                callPosition(state, "skiq_write_1pps_source"));
        REQUIRE(callPosition(state, "skiq_write_1pps_source") <
                callPosition(state, "skiq_register_tx_complete_callback"));
        REQUIRE(std::find(state.calls.begin(), state.calls.end(),
                          "skiq_read_last_1pps_timestamp") == state.calls.end());
    }
    REQUIRE_EQ(scripted_sdk::snapshot().exit_calls, 1u);
}

TEST_CASE("concrete optional-constructor failures roll back and permit a clean retry")
{
    for (const auto *failure : {"unsupported topology", "skiq_apply_topology",
                                "skiq_write_ref_clock_select", "skiq_read_last_1pps_timestamp"})
    {
        scripted_sdk::reset(failure);
        const bool unsupported = std::string(failure) == "unsupported topology";
        if (unsupported)
        {
            std::lock_guard<std::mutex> lock(scripted_sdk::mutex);
            scripted_sdk::script.topology_supported = false;
        }
        auto args = arguments();
        args["topology"] = "17";
        args["clock_source"] = "external_clock";
        args["time_source"] = "1pps_source_external";
        if (unsupported)
            REQUIRE_THROWS_AS(std::unique_ptr<SoapySDR::Device>(makeSidekiq(args)),
                              std::invalid_argument);
        else
            REQUIRE_THROWS_AS(std::unique_ptr<SoapySDR::Device>(makeSidekiq(args)),
                              std::runtime_error);
        const auto failed = scripted_sdk::snapshot();
        REQUIRE(!failed.initialized && failed.callback == nullptr);
        REQUIRE(failed.pending.empty());
        REQUIRE_EQ(failed.init_calls, 1u);
        REQUIRE_EQ(failed.exit_calls, 1u);
        REQUIRE_EQ(failed.calls.back(), std::string("skiq_exit"));
        REQUIRE(std::find(failed.calls.begin(), failed.calls.end(),
                          "skiq_register_tx_complete_callback") == failed.calls.end());
        if (unsupported)
            REQUIRE(std::find(failed.calls.begin(), failed.calls.end(), "skiq_apply_topology") ==
                    failed.calls.end());
        else
            REQUIRE(callPosition(failed, failure) < callPosition(failed, "skiq_exit"));

        // A failed source observation must not retain SDK/session ownership.
        // Disable PPS observation on retry: this test has no physical PPS input.
        scripted_sdk::reset();
        args["time_source"] = "1pps_source_unavailable";
        {
            std::unique_ptr<SoapySDR::Device> device(makeSidekiq(args));
            REQUIRE(scripted_sdk::snapshot().initialized);
            REQUIRE(scripted_sdk::snapshot().callback != nullptr);
        }
        REQUIRE(!scripted_sdk::snapshot().initialized);
        REQUIRE_EQ(scripted_sdk::snapshot().exit_calls, 1u);
    }
}

TEST_CASE("concrete device teardown permits callbacks on another thread")
{
    for (unsigned cycle = 0; cycle < 16; ++cycle)
    {
        scripted_sdk::reset();
        soapy_sidekiq::ScopeExit deliver([]() noexcept { scripted_sdk::completeAll(); });
        std::unique_ptr<SoapySDR::Device> device(makeSidekiq(arguments()));
        auto *tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, {0});
        REQUIRE_EQ(device->activateStream(tx), 0);
        const auto mtu = device->getStreamMTU(tx);
        std::vector<std::int16_t> input(4 * mtu, 37);
        const void *buffers[]{input.data()};
        int flags = 0;
        REQUIRE_EQ(device->writeStream(tx, buffers, 2 * mtu, flags, 0, 0),
                   static_cast<int>(2 * mtu));
        REQUIRE_EQ(scripted_sdk::snapshot().pending.size(), std::size_t{2});

        // The future joins on assertion/exception unwinding as well. Delivery
        // may finish before, during, or after the real device's teardown.
        auto completions = std::async(std::launch::async, [] { scripted_sdk::completeAll(); });
        device.reset();
        completions.get();
        const auto stopped = scripted_sdk::snapshot();
        REQUIRE(!stopped.initialized && stopped.callback == nullptr);
        REQUIRE(stopped.pending.empty());
        REQUIRE_EQ(stopped.completions, 2u);
        REQUIRE_EQ(stopped.tx_stops, 1u);
        REQUIRE_EQ(stopped.exit_calls, 1u);
    }
}
