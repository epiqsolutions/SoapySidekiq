/**
 * @file SampleConversionTests.cpp
 * @brief Verifies CF32 clipping, rounding, validation, and CS16 layout.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/SampleConversion.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

using soapy_sidekiq::convertCf32ToCs16;

// Cover normal values and every exceptional-value policy used by TX streaming.
TEST_CASE("CF32 samples convert to signed CS16 pairs")
{
    const std::array<float, 6> input{{1.0F, -1.0F, 0.0F, 0.5F, -0.5F, 0.25F}};
    std::array<std::int16_t, 6> output{};

    convertCf32ToCs16(input.data(), output.data(), 3, 2047.0F);

    REQUIRE_EQ(output[0], std::int16_t{2047});
    REQUIRE_EQ(output[1], std::int16_t{-2047});
    REQUIRE_EQ(output[2], std::int16_t{0});
    REQUIRE_EQ(output[3], std::int16_t{1024});
    REQUIRE_EQ(output[4], std::int16_t{-1024});
    REQUIRE_EQ(output[5], std::int16_t{512});
}

TEST_CASE("CF32 conversion clips values outside the normalized range")
{
    const std::array<float, 4> input{{2.0F, -3.0F,
                                      std::numeric_limits<float>::infinity(),
                                      -std::numeric_limits<float>::infinity()}};
    std::array<std::int16_t, 4> output{};

    convertCf32ToCs16(input.data(), output.data(), 2, 2047.0F);

    REQUIRE_EQ(output[0], std::int16_t{2047});
    REQUIRE_EQ(output[1], std::int16_t{-2047});
    REQUIRE_EQ(output[2], std::int16_t{2047});
    REQUIRE_EQ(output[3], std::int16_t{-2047});
}

TEST_CASE("CF32 conversion maps NaN to a deterministic zero sample")
{
    const std::array<float, 2> input{{std::numeric_limits<float>::quiet_NaN(), 0.25F}};
    std::array<std::int16_t, 2> output{};

    convertCf32ToCs16(input.data(), output.data(), 1, 2047.0F);

    REQUIRE_EQ(output[0], std::int16_t{0});
    REQUIRE_EQ(output[1], std::int16_t{512});
}

TEST_CASE("CF32 conversion validates scale and buffer pointers")
{
    std::array<float, 2> input{};
    std::array<std::int16_t, 2> output{};

    REQUIRE_THROWS_AS(
        convertCf32ToCs16(input.data(), output.data(), 1, 0.0F),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        convertCf32ToCs16(
            input.data(), output.data(), 1,
            std::numeric_limits<float>::infinity()),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        convertCf32ToCs16(nullptr, output.data(), 1, 2047.0F),
        std::invalid_argument);

    convertCf32ToCs16(nullptr, nullptr, 0, 2047.0F);
}
