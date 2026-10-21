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

TEST_CASE("actual fractional RF rates preserve sub-Hz clock precision")
{
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(1, 1.5), std::int64_t{666666666});
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(3, 1.5), std::int64_t{2000000000});
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(0, 1.5), std::int64_t{0});
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(100000005, 10000000.5),
               std::int64_t{10000000000});
}
TEST_CASE("actual integral RF rates retain exact large counter conversion")
{
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(9007199254740993ULL, 1000000000.0),
               std::int64_t{9007199254740993LL});
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    REQUIRE_EQ(soapy_sidekiq::ticksToNanosecondsAtRate(maximum, 1000000000.0),
               std::numeric_limits<std::int64_t>::max());
    REQUIRE_THROWS_AS(soapy_sidekiq::ticksToNanosecondsAtRate(maximum + 1, 1000000000.0),
                      std::overflow_error);
}
TEST_CASE("actual RF rate conversion rejects invalid frequencies and overflowing results")
{
    for (const auto frequency : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()})
        REQUIRE_THROWS_AS(soapy_sidekiq::ticksToNanosecondsAtRate(1, frequency),
                          std::invalid_argument);
    REQUIRE_THROWS_AS(
        soapy_sidekiq::ticksToNanosecondsAtRate(std::numeric_limits<std::uint64_t>::max(), 1.5),
        std::overflow_error);
}

TEST_CASE("nanosecond to tick conversion preserves exact integer precision")
{
    using soapy_sidekiq::nanosecondsToTicks;
    REQUIRE_EQ(nanosecondsToTicks(0, 40000000), std::uint64_t{0});
    REQUIRE_EQ(nanosecondsToTicks(1, 40000000), std::uint64_t{0});
    REQUIRE_EQ(nanosecondsToTicks(25, 40000000), std::uint64_t{1});
    REQUIRE_EQ(nanosecondsToTicks(1000000001, 3), std::uint64_t{3});
    REQUIRE_EQ(nanosecondsToTicks(9007199254740993LL, 1000000000),
               std::uint64_t{9007199254740993ULL});
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_EQ(nanosecondsToTicks(1000000000, maximum), maximum);
    REQUIRE_EQ(nanosecondsToTicks(999999999, maximum), std::uint64_t{18446744055262807541ULL});
}
TEST_CASE("nanosecond to tick conversion rejects invalid inputs and exact overflow")
{
    using soapy_sidekiq::nanosecondsToTicks;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_THROWS_AS(nanosecondsToTicks(-1, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(nanosecondsToTicks(0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(nanosecondsToTicks(2000000000, maximum), std::overflow_error);
    REQUIRE_THROWS_AS(nanosecondsToTicks(1000000001, maximum), std::overflow_error);
    REQUIRE_EQ(nanosecondsToTicks(std::numeric_limits<std::int64_t>::max(), 1000000000),
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}
#if defined(__SIZEOF_INT128__)
TEST_CASE("nanosecond to tick conversion agrees with a wide integer oracle")
{
    __extension__ using uint128 = unsigned __int128;
    std::uint64_t state = 0x8b73ac19b2d49317ULL;
    const auto next = [&state]()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (unsigned trial = 0; trial < 10000; ++trial)
    {
        // The mask proves the explicit signed conversion is representable.
        const auto time = static_cast<std::int64_t>(
            next() & static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        const auto frequency = next() | 1ULL;
        const auto expected = static_cast<uint128>(time) * frequency / 1000000000ULL;
        if (expected > std::numeric_limits<std::uint64_t>::max())
            REQUIRE_THROWS_AS(soapy_sidekiq::nanosecondsToTicks(time, frequency),
                              std::overflow_error);
        else
            REQUIRE_EQ(soapy_sidekiq::nanosecondsToTicks(time, frequency),
                       static_cast<std::uint64_t>(expected));
    }
}
#endif
