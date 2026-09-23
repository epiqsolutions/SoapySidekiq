#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace soapy_sidekiq
{

inline std::int64_t ticksToNanoseconds(
    const std::uint64_t ticks,
    const std::uint64_t frequency)
{
    if (frequency == 0)
    {
        throw std::invalid_argument("timestamp frequency cannot be zero");
    }

#if defined(__SIZEOF_INT128__)
    __extension__ using uint128 = unsigned __int128;
    const auto nanoseconds =
        (static_cast<uint128>(ticks) * 1000000000ULL) / frequency;
    if (nanoseconds >
        static_cast<uint128>(std::numeric_limits<std::int64_t>::max()))
    {
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    }
    return static_cast<std::int64_t>(nanoseconds);
#else
    constexpr std::uint64_t nanoseconds_per_second = 1000000000ULL;
    const std::uint64_t whole_seconds = ticks / frequency;
    const std::uint64_t remaining_ticks = ticks % frequency;
    if (whole_seconds >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
            nanoseconds_per_second)
    {
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    }

    const long double fractional_nanoseconds =
        static_cast<long double>(remaining_ticks) * nanoseconds_per_second /
        static_cast<long double>(frequency);
    const auto whole_nanoseconds = whole_seconds * nanoseconds_per_second;
    const auto fractional = static_cast<std::uint64_t>(fractional_nanoseconds);
    if (fractional >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
            whole_nanoseconds)
    {
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    }
    return static_cast<std::int64_t>(whole_nanoseconds + fractional);
#endif
}

} // namespace soapy_sidekiq
