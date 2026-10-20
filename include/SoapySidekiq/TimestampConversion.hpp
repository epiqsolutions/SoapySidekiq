/**
 * @file TimestampConversion.hpp
 * @brief Provides overflow-checked conversion from device ticks to nanoseconds.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace soapy_sidekiq
{

/**
 * Convert counter ticks to nanoseconds using truncating integer division.
 *
 * The implementation avoids floating-point precision loss for large counters
 * and rejects results that cannot fit the signed SoapySDR timestamp type.
 *
 * @param ticks Device counter value to convert.
 * @param frequency Counter ticks per second.
 * @return Whole nanoseconds, truncated toward zero.
 * @throws std::invalid_argument when frequency is zero.
 * @throws std::overflow_error when the nanosecond result exceeds int64_t.
 */
inline std::int64_t ticksToNanoseconds(
    const std::uint64_t ticks,
    const std::uint64_t frequency)
{
    if (frequency == 0)
    {
        throw std::invalid_argument("timestamp frequency cannot be zero");
    }

#if defined(__SIZEOF_INT128__) && !defined(SOAPYSIDEKIQ_FORCE_PORTABLE_TIMESTAMP)
    // Widen before multiplication so the exact numerator cannot wrap uint64_t.
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
    // Split whole and fractional seconds on compilers without 128-bit integers.
    constexpr std::uint64_t nanoseconds_per_second = 1000000000ULL;
    const std::uint64_t whole_seconds = ticks / frequency;
    const std::uint64_t remaining_ticks = ticks % frequency;
    if (whole_seconds >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
            nanoseconds_per_second)
    {
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    }

    // Binary long division computes remaining_ticks * 1e9 / frequency without
    // forming the overflowing product. After each bit, remainder < frequency
    // and fractional is the quotient for the multiplier prefix processed so far.
    std::uint64_t remainder = 0;
    std::uint64_t fractional = 0;
    for (std::uint32_t bit = 1U << 29; bit != 0; bit >>= 1)
    {
        fractional *= 2;
        // Compare before doubling so even frequencies near UINT64_MAX are safe.
        if (remainder >= frequency - remainder)
        {
            remainder -= frequency - remainder;
            ++fractional;
        }
        else
        {
            remainder *= 2;
        }
        if ((nanoseconds_per_second & bit) != 0)
        {
            // Add the next numerator contribution without overflowing uint64_t.
            if (remainder >= frequency - remaining_ticks)
            {
                remainder -= frequency - remaining_ticks;
                ++fractional;
            }
            else
            {
                remainder += remaining_ticks;
            }
        }
    }
    const auto whole_nanoseconds = whole_seconds * nanoseconds_per_second;
    if (fractional >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
            whole_nanoseconds)
    {
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    }
    return static_cast<std::int64_t>(whole_nanoseconds + fractional);
#endif
}

/**
 * Convert RF ticks using the SDK's actual, possibly fractional, sample rate.
 * Integer rates use the exact integer conversion; fractional rates use widened
 * floating-point division and reject results outside the signed timestamp range.
 * @param ticks Device counter value.
 * @param frequency Actual counter ticks per second, finite and positive.
 * @return Whole nanoseconds, truncated toward zero.
 * @throws std::invalid_argument for invalid frequency.
 * @throws std::overflow_error for an unrepresentable timestamp.
 */
inline std::int64_t ticksToNanosecondsAtRate(const std::uint64_t ticks, const double frequency)
{
    if (!std::isfinite(frequency) || frequency <= 0)
        throw std::invalid_argument("timestamp rate must be finite and positive");
    if (std::floor(frequency) == frequency && frequency < std::ldexp(1.0, 64))
        return ticksToNanoseconds(ticks, static_cast<std::uint64_t>(frequency));
    const long double nanoseconds =
        static_cast<long double>(ticks) * 1000000000.0L / static_cast<long double>(frequency);
    // The exclusive power-of-two limit remains safe when long double == double.
    if (!std::isfinite(nanoseconds) || nanoseconds >= std::ldexp(1.0L, 63))
        throw std::overflow_error("timestamp does not fit in nanoseconds");
    return static_cast<std::int64_t>(nanoseconds);
}

/**
 * Convert nonnegative nanoseconds to device ticks, truncating fractional ticks.
 * Split seconds and frequency before multiplication so all intermediates fit
 * uint64_t without relying on compiler-specific wide integer support.
 * @param nanoseconds Requested SoapySDR time.
 * @param frequency Device counter ticks per second.
 * @return Whole device ticks.
 * @throws std::invalid_argument for negative time or zero frequency.
 * @throws std::overflow_error when the tick count exceeds uint64_t.
 */
inline std::uint64_t nanosecondsToTicks(const std::int64_t nanoseconds,
                                        const std::uint64_t frequency)
{
    if (nanoseconds < 0 || frequency == 0)
        throw std::invalid_argument("timestamp requires nonnegative time and nonzero frequency");
    constexpr std::uint64_t billion = 1000000000ULL;
    const auto time = static_cast<std::uint64_t>(nanoseconds);
    const auto seconds = time / billion;
    const auto remainder = time % billion;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (seconds > maximum / frequency)
        throw std::overflow_error("timestamp does not fit in device ticks");
    const auto whole = seconds * frequency;
    // Each product is bounded: remainder < 1e9 and frequency % 1e9 < 1e9.
    const auto fractional =
        (frequency / billion) * remainder + ((frequency % billion) * remainder) / billion;
    if (fractional > maximum - whole)
        throw std::overflow_error("timestamp does not fit in device ticks");
    return whole + fractional;
}

} // namespace soapy_sidekiq
