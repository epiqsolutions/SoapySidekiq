/**
 * @file StreamValidationTests.cpp
 * @brief Checks concrete stream request validation before SDK side effects.
 */
#include "FakeSdk.hpp"
#include "TestHarness.hpp"
#include "SoapySidekiq.hpp"
#include <SoapySDR/Constants.h>
#include <SoapySDR/Formats.h>
#include <stdexcept>

TEST_CASE("concrete stream setup rejects unsupported keywords without SDK calls")
{
    scripted_sdk::reset();
    SoapySidekiq device({{"card", "5"}, {"tx_buffer_count", "2"}, {"tx_block_size", "252"}});
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX})
        for (const auto *format : {SOAPY_SDR_CS16, SOAPY_SDR_CF32})
        {
            REQUIRE(device.getStreamArgsInfo(direction, 0).empty());
            for (const auto *key : {"bufflen", "tx_block_size", "unknown"})
            {
                const auto calls_before = scripted_sdk::snapshot().calls;
                const auto setup = [&]
                {
                    // Close an unexpectedly accepted stream before reporting failure.
                    auto *stream = device.setupStream(direction, format, {0}, {{key, "1024"}});
                    device.closeStream(stream);
                };
                REQUIRE_THROWS_AS(setup(), std::invalid_argument);
                REQUIRE(scripted_sdk::snapshot().calls == calls_before);
                auto *stream = device.setupStream(direction, format, {0});
                REQUIRE(device.getStreamMTU(stream) > 0);
                device.closeStream(stream);
            }
        }
}
