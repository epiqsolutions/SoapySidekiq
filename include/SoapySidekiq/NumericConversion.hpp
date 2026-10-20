/**
 * @file NumericConversion.hpp
 * @brief Checks floating-point SDK arguments before unsigned integer conversion.
 */

#pragma once

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace soapy_sidekiq
{
/**
 * Convert a finite nonnegative SDK argument, truncating fractional units.
 * Use the exclusive power-of-two limit: converting an integer maximum to
 * double may round it up and accidentally admit an out-of-range value.
 * @tparam Integer Unsigned integer SDK parameter type.
 * @param value Requested value in the SDK's units.
 * @param operation Operation/parameter included in validation failures.
 * @return Representable integer, with fractional units truncated.
 * @throws std::invalid_argument for non-finite, negative, or overflowing input.
 */
template <typename Integer>
Integer checkedUnsigned(const double value, const std::string &operation)
{
    static_assert(std::is_integral_v<Integer> && std::is_unsigned_v<Integer>);
    const double limit = std::ldexp(1.0, std::numeric_limits<Integer>::digits);
    if (!std::isfinite(value) || value < 0 || value >= limit)
        throw std::invalid_argument(operation + " value " + std::to_string(value) +
                                    " is outside the unsigned SDK parameter range");
    return static_cast<Integer>(value);
}
} // namespace soapy_sidekiq
