/**
 * @file TimestampConversionTests.cpp
 * @brief Verifies exact, fractional, invalid, and overflowing tick conversion.
 */

#include "TestHarness.hpp"

#include <SoapySidekiq/TimestampConversion.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

using soapy_sidekiq::ticksToNanoseconds;

// Exercise exact, fractional, large-counter, invalid, and overflow paths.
TEST_CASE("timestamp conversion handles exact sample-rate intervals")
{
    REQUIRE_EQ(ticksToNanoseconds(0, 20000000), std::int64_t{0});
    REQUIRE_EQ(ticksToNanoseconds(1, 20000000), std::int64_t{50});
    REQUIRE_EQ(ticksToNanoseconds(30000000, 20000000), std::int64_t{1500000000});
}

TEST_CASE("timestamp conversion truncates fractional nanoseconds consistently")
{
    REQUIRE_EQ(ticksToNanoseconds(1, 3), std::int64_t{333333333});
    REQUIRE_EQ(ticksToNanoseconds(2, 3), std::int64_t{666666666});
    REQUIRE_EQ(ticksToNanoseconds(3, 3), std::int64_t{1000000000});
}

TEST_CASE("timestamp conversion remains exact for large fractional-rate counters")
{
    constexpr std::uint64_t ticks = 10000000000000123ULL;
    constexpr std::uint64_t frequency = 122880000ULL;
    REQUIRE_EQ(ticksToNanoseconds(ticks, frequency),
               std::int64_t{81380208333334334LL});
}

TEST_CASE("timestamp conversion rejects invalid frequency and overflow")
{
    REQUIRE_THROWS_AS(ticksToNanoseconds(1, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(
        ticksToNanoseconds(std::numeric_limits<std::uint64_t>::max(), 1),
        std::overflow_error);
}

TEST_CASE("timestamp conversion truncates values immediately below integer boundaries")
{
    REQUIRE_EQ(ticksToNanoseconds(666666667000000002ULL, 1000000000000000003ULL),
               std::int64_t{666666666});
    REQUIRE_EQ(ticksToNanoseconds(34052506, 122880001), std::int64_t{277120000});
    REQUIRE_EQ(ticksToNanoseconds(6396716143909704276ULL, 18446744073709551613ULL),
               std::int64_t{346766676});
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_EQ(ticksToNanoseconds(maximum, maximum), std::int64_t{1000000000});
    REQUIRE_EQ(ticksToNanoseconds(maximum - 1, maximum), std::int64_t{999999999});
}

TEST_CASE("timestamp conversion checks the exact signed nanosecond limit")
{
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    REQUIRE_EQ(ticksToNanoseconds(maximum, 1000000000),
               std::numeric_limits<std::int64_t>::max());
    // Exercise overflow caused by adding the fractional-second contribution.
    REQUIRE_THROWS_AS(ticksToNanoseconds(maximum + 1, 1000000000), std::overflow_error);
    REQUIRE_THROWS_AS(ticksToNanoseconds(maximum, 999999999), std::overflow_error);
    REQUIRE_THROWS_AS(ticksToNanoseconds(9223372037ULL, 1), std::overflow_error);
}

#if defined(__SIZEOF_INT128__)
TEST_CASE("timestamp conversion agrees with a wide integer oracle across counter ranges")
{
    __extension__ using uint128 = unsigned __int128;
    std::uint64_t state = 0x934b671ad259804fULL;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (unsigned trial = 0; trial < 10000; ++trial)
    {
        const auto ticks = next();
        // Alternate full-width and small frequencies to cover overflow too.
        const auto random_frequency = next();
        const auto frequency = (trial % 2 == 0 ? random_frequency :
                                random_frequency % 1000000000) | 1ULL;
        const auto expected = static_cast<uint128>(ticks) * 1000000000 / frequency;
        if (expected > static_cast<uint128>(std::numeric_limits<std::int64_t>::max()))
        {
            REQUIRE_THROWS_AS(ticksToNanoseconds(ticks, frequency), std::overflow_error);
        }
        else
        {
            REQUIRE_EQ(ticksToNanoseconds(ticks, frequency), static_cast<std::int64_t>(expected));
        }
    }
}
#endif
