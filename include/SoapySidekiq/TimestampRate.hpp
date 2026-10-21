/**
 * @file TimestampRate.hpp
 * @brief Represents fractional SDK clock rates and exact nanosecond phases.
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace soapy_sidekiq
{
namespace detail
{
/** Quotient and remainder of an overflow-safe product division. */
struct ProductDivision
{
    /** Whole result of the scaled integer division. */
    std::uint64_t quotient;
    /** Remainder in units of the supplied divisor. */
    std::uint64_t remainder;
};
/** Compute value * multiplier / divisor; value must be strictly below divisor. */
inline ProductDivision multiplyDivide(std::uint64_t value, std::uint64_t multiplier,
                                      std::uint64_t divisor)
{
#if defined(__SIZEOF_INT128__) && !defined(SOAPYSIDEKIQ_FORCE_PORTABLE_TIMESTAMP)
    __extension__ using uint128 = unsigned __int128;
    const auto product = static_cast<uint128>(value) * multiplier;
    return {static_cast<std::uint64_t>(product / divisor),
            static_cast<std::uint64_t>(product % divisor)};
#else
    ProductDivision result{0, 0};
    for (std::uint64_t bit = 1ULL << 63; bit != 0; bit >>= 1)
    {
        result.quotient *= 2;
        if (result.remainder >= divisor - result.remainder)
        {
            result.remainder -= divisor - result.remainder;
            ++result.quotient;
        }
        else
            result.remainder *= 2;
        if ((multiplier & bit) != 0)
        {
            if (result.remainder >= divisor - value)
            {
                result.remainder -= divisor - value;
                ++result.quotient;
            }
            else
                result.remainder += value;
        }
    }
    return result;
#endif
}
} // namespace detail

/** Whole nanoseconds and the exact fractional nanosecond remainder. */
struct TimestampPhase
{
    /** Whole signed nanoseconds after truncation. */
    std::int64_t nanoseconds;
    /** Fractional nanosecond numerator retained for later sample offsets. */
    std::uint64_t remainder;
    /** Positive divisor for the fractional nanosecond remainder. */
    std::uint64_t denominator;
};

/** Exact positive rate, represented as numerator/denominator ticks per second. */
class TimestampRate
{
  public:
    /** Integer clocks remain exact across the full uint64 range; zero is unset. */
    TimestampRate(std::uint64_t frequency = 0) : numerator_(frequency) {}

    /**
     * Preserve the exact binary value of an SDK actual RF rate.
     * @param frequency Finite rate in the SDK's [1, 2^32) sample/second range.
     * @return Reduced rational representation without rounding fractional Hz.
     * @throws std::invalid_argument for an invalid or out-of-range rate.
     */
    static TimestampRate fromActualRate(double frequency)
    {
        if (!std::isfinite(frequency) || frequency < 1 || frequency >= std::ldexp(1.0, 32))
            throw std::invalid_argument("Actual RF rate must be finite and in [1, 2^32)");
        static_assert(std::numeric_limits<double>::radix == 2 &&
                      std::numeric_limits<double>::digits <= 64);
        int exponent = 0;
        const double mantissa = std::frexp(frequency, &exponent);
        constexpr int digits = std::numeric_limits<double>::digits;
        TimestampRate result;
        result.numerator_ = static_cast<std::uint64_t>(std::ldexp(mantissa, digits));
        if (exponent >= digits)
            result.numerator_ <<= exponent - digits;
        else
            result.denominator_ <<= digits - exponent;
        while (result.denominator_ > 1 && (result.numerator_ & 1) == 0)
        {
            result.numerator_ >>= 1;
            result.denominator_ >>= 1;
        }
        return result;
    }
    /** @return Whether a nonzero rate was supplied. */
    bool valid() const
    {
        return numerator_ != 0;
    }

    /**
     * Convert ticks while retaining their exact fractional nanosecond phase.
     * @param ticks Counter ticks or complex-sample offset.
     * @return Truncated nanoseconds and remainder/denominator fraction.
     * @throws std::invalid_argument for an unset rate.
     * @throws std::overflow_error when nanoseconds exceed int64_t.
     */
    TimestampPhase convert(std::uint64_t ticks) const
    {
        if (!valid())
            throw std::invalid_argument("Timestamp rate cannot be zero");
        constexpr std::uint64_t billion = 1000000000ULL;
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto seconds_limit = maximum / billion;
        const auto whole = ticks / numerator_;
        if (whole > seconds_limit / denominator_)
            throw std::overflow_error("Timestamp does not fit in nanoseconds");
        auto seconds = whole * denominator_;
        const auto fraction = detail::multiplyDivide(ticks % numerator_, denominator_, numerator_);
        if (fraction.quotient > seconds_limit - seconds)
            throw std::overflow_error("Timestamp does not fit in nanoseconds");
        seconds += fraction.quotient;
        const auto phase = detail::multiplyDivide(fraction.remainder, billion, numerator_);
        const auto nanoseconds = seconds * billion;
        if (phase.quotient > maximum - nanoseconds)
            throw std::overflow_error("Timestamp does not fit in nanoseconds");
        return {static_cast<std::int64_t>(nanoseconds + phase.quotient), phase.remainder,
                numerator_};
    }

  private:
    /** Reduced samples/ticks numerator; zero denotes an unspecified rate. */
    std::uint64_t numerator_{};
    /** Positive reduced denominator paired with numerator_. */
    std::uint64_t denominator_{1};
};
} // namespace soapy_sidekiq
