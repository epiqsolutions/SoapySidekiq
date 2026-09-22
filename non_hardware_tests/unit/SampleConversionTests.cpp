#include "TestHarness.hpp"

#include <SoapySidekiq/SampleConversion.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

using soapy_sidekiq::convertCf32ToCs16;
using soapy_sidekiq::convertCs16ToCf32;

TEST_CASE("CS16 samples convert to normalized CF32 pairs")
{
    const std::array<std::int16_t, 6> input{{2047, -2047, 0, 1024, -1024, 1}};
    std::array<float, 6> output{};

    convertCs16ToCf32(input.data(), output.data(), 3, 2047.0F);

    REQUIRE_EQ(output[0], 1.0F);
    REQUIRE_EQ(output[1], -1.0F);
    REQUIRE(std::abs(output[3] - (1024.0F / 2047.0F)) < 0.000001F);
}

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

TEST_CASE("CF32 conversion clips out-of-range values and handles NaN")
{
    const std::array<float, 4> input{{2.0F, -3.0F,
                                      std::numeric_limits<float>::infinity(),
                                      std::numeric_limits<float>::quiet_NaN()}};
    std::array<std::int16_t, 4> output{};

    convertCf32ToCs16(input.data(), output.data(), 2, 2047.0F);

    REQUIRE_EQ(output[0], std::int16_t{2047});
    REQUIRE_EQ(output[1], std::int16_t{-2047});
    REQUIRE_EQ(output[2], std::int16_t{2047});
    REQUIRE_EQ(output[3], std::int16_t{0});
}

TEST_CASE("sample conversion validates full scale and buffer pointers")
{
    std::array<std::int16_t, 2> samples{};
    std::array<float, 2> floats{};

    REQUIRE_THROWS_AS(
        convertCs16ToCf32(samples.data(), floats.data(), 1, 0.0F),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        convertCs16ToCf32(nullptr, floats.data(), 1, 2047.0F),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        convertCf32ToCs16(nullptr, samples.data(), 1, 2047.0F),
        std::invalid_argument);

    convertCs16ToCf32(nullptr, nullptr, 0, 2047.0F);
    convertCf32ToCs16(nullptr, nullptr, 0, 2047.0F);
}
