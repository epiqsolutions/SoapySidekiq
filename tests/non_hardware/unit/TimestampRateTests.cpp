/**
 * @file TimestampRateTests.cpp
 * @brief Verifies exact fractional clock arithmetic and portable overflow checks.
 */
#include "TestHarness.hpp"
#include <SoapySidekiq/TimestampRate.hpp>
#include <limits>
using soapy_sidekiq::TimestampRate;
TEST_CASE("fractional rates retain exact binary clock phase")
{
    const auto rate = TimestampRate::fromActualRate(1.5);
    const auto phase = rate.convert(1);
    REQUIRE_EQ(phase.nanoseconds, std::int64_t{666666666});
    REQUIRE_EQ(phase.remainder, std::uint64_t{2});
    REQUIRE_EQ(phase.denominator, std::uint64_t{3});
    REQUIRE_EQ(rate.convert(3).nanoseconds, std::int64_t{2000000000});
    REQUIRE_EQ(rate.convert(3).remainder, std::uint64_t{0});
    REQUIRE_EQ(TimestampRate::fromActualRate(10000000.5).convert(100000005).nanoseconds,
               std::int64_t{10000000000});
    REQUIRE_EQ(TimestampRate::fromActualRate(std::nextafter(1.0, 2.0)).convert(1).nanoseconds,
               std::int64_t{999999999});
}
TEST_CASE("timestamp rates reject invalid actual SDK rates and unset clocks")
{
    for (const auto rate :
         {0.0, -1.0, 0.5, std::ldexp(1.0, 32), std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
        REQUIRE_THROWS_AS(TimestampRate::fromActualRate(rate), std::invalid_argument);
    REQUIRE_THROWS_AS(TimestampRate{}.convert(0), std::invalid_argument);
}
TEST_CASE("timestamp phase conversion checks exact signed boundaries")
{
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    REQUIRE_EQ(TimestampRate(1000000000).convert(maximum).nanoseconds,
               std::numeric_limits<std::int64_t>::max());
    REQUIRE_THROWS_AS(TimestampRate(1000000000).convert(maximum + 1), std::overflow_error);
    REQUIRE_THROWS_AS(TimestampRate::fromActualRate(1.5).convert(maximum), std::overflow_error);
    REQUIRE_EQ(TimestampRate(std::numeric_limits<std::uint64_t>::max())
                   .convert(std::numeric_limits<std::uint64_t>::max())
                   .nanoseconds,
               std::int64_t{1000000000});
}
#if defined(__SIZEOF_INT128__)
TEST_CASE("fractional timestamp phases agree with a wide integer oracle")
{
    __extension__ using uint128 = unsigned __int128;
    std::uint64_t state = 0x475ab2fe18294ac3ULL;
    const auto next = [&state]()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (unsigned trial = 0; trial < 10000; ++trial)
    {
        const std::uint64_t numerator = 10000000ULL * 1024 + (next() % 1024);
        const auto ticks = next();
        const auto product = static_cast<uint128>(ticks) * 1024 * 1000000000;
        const auto expected = product / numerator;
        const auto rate = TimestampRate::fromActualRate(static_cast<double>(numerator) / 1024);
        if (expected > static_cast<uint128>(std::numeric_limits<std::int64_t>::max()))
            REQUIRE_THROWS_AS(rate.convert(ticks), std::overflow_error);
        else
        {
            const auto result = rate.convert(ticks);
            REQUIRE_EQ(result.nanoseconds, static_cast<std::int64_t>(expected));
            REQUIRE(static_cast<uint128>(result.remainder) * numerator ==
                    (product % numerator) * result.denominator);
        }
    }
}
#endif
