#include "TestHarness.hpp"

#include <SoapySidekiq/TimestampConversion.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

using soapy_sidekiq::ticksToNanoseconds;

TEST_CASE("timestamp conversion handles exact sample-rate intervals")
{
    REQUIRE_EQ(ticksToNanoseconds(0, 20000000), std::int64_t{0});
    REQUIRE_EQ(ticksToNanoseconds(1, 20000000), std::int64_t{50});
    REQUIRE_EQ(ticksToNanoseconds(20000000, 20000000), std::int64_t{1000000000});
    REQUIRE_EQ(ticksToNanoseconds(30000000, 20000000), std::int64_t{1500000000});
}

TEST_CASE("timestamp conversion truncates fractional nanoseconds consistently")
{
    REQUIRE_EQ(ticksToNanoseconds(1, 3), std::int64_t{333333333});
    REQUIRE_EQ(ticksToNanoseconds(2, 3), std::int64_t{666666666});
    REQUIRE_EQ(ticksToNanoseconds(3, 3), std::int64_t{1000000000});
}

TEST_CASE("timestamp conversion remains exact for large counters")
{
    constexpr std::uint64_t ticks = 172567433904834834ULL;
    constexpr std::uint64_t frequency = 20000000ULL;
    REQUIRE_EQ(ticksToNanoseconds(ticks, frequency),
               std::int64_t{8628371695241741700LL});
}

TEST_CASE("timestamp conversion rejects invalid frequency and overflow")
{
    REQUIRE_THROWS_AS(ticksToNanoseconds(1, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(
        ticksToNanoseconds(std::numeric_limits<std::uint64_t>::max(), 1),
        std::overflow_error);
}
