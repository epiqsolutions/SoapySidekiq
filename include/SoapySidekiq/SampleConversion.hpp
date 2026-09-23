/**
 * @file SampleConversion.hpp
 * @brief Provides deterministic, hardware-independent sample conversion helpers.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace soapy_sidekiq
{

/**
 * Convert one normalized floating-point component to a signed integer sample.
 *
 * Values outside [-1, 1] are clipped, infinities clip to their corresponding
 * endpoint, and NaN maps deterministically to zero.
 *
 * @param value Normalized floating-point component to convert.
 * @param full_scale Positive integer-domain full-scale magnitude.
 * @return Rounded and clipped signed 16-bit component.
 * @throws std::invalid_argument when full_scale is non-finite, non-positive,
 * or larger than the signed 16-bit range.
 */
inline std::int16_t convertNormalizedFloatToSample(
    const float value,
    const float full_scale)
{
    if (!std::isfinite(full_scale) || full_scale <= 0.0F ||
        full_scale > static_cast<float>(std::numeric_limits<std::int16_t>::max()))
    {
        throw std::invalid_argument("sample full scale is outside the supported range");
    }
    if (std::isnan(value))
    {
        // Avoid implementation-defined floating-to-integer handling for NaN.
        return 0;
    }

    const float clipped = std::clamp(value, -1.0F, 1.0F);
    return static_cast<std::int16_t>(std::lround(clipped * full_scale));
}

/**
 * Convert interleaved normalized CF32 IQ pairs to interleaved CS16 IQ pairs.
 * Zero samples permit null buffers; non-empty conversions require both buffers.
 *
 * @param input Source buffer containing two floats per complex sample.
 * @param output Destination buffer receiving two int16 values per sample.
 * @param complex_samples Number of complex samples to convert.
 * @param full_scale Positive integer-domain full-scale magnitude.
 * @throws std::invalid_argument for invalid scale or nonempty null buffers.
 */
inline void convertCf32ToCs16(
    const float *input,
    std::int16_t *output,
    const std::size_t complex_samples,
    const float full_scale)
{
    if (complex_samples != 0 && (input == nullptr || output == nullptr))
    {
        throw std::invalid_argument("sample buffers cannot be null");
    }

    for (std::size_t index = 0; index < complex_samples * 2; ++index)
    {
        output[index] = convertNormalizedFloatToSample(input[index], full_scale);
    }
}

} // namespace soapy_sidekiq
