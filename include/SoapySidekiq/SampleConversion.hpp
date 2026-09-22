#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace soapy_sidekiq
{

inline void convertCs16ToCf32(
    const std::int16_t *input,
    float *output,
    const std::size_t complex_samples,
    const float full_scale)
{
    if (full_scale <= 0.0F)
    {
        throw std::invalid_argument("sample full scale must be positive");
    }
    if (complex_samples != 0 && (input == nullptr || output == nullptr))
    {
        throw std::invalid_argument("sample buffers cannot be null");
    }

    for (std::size_t index = 0; index < complex_samples * 2; ++index)
    {
        output[index] = static_cast<float>(input[index]) / full_scale;
    }
}

inline std::int16_t convertNormalizedFloatToSample(
    const float value,
    const float full_scale)
{
    if (full_scale <= 0.0F)
    {
        throw std::invalid_argument("sample full scale must be positive");
    }
    if (std::isnan(value))
    {
        return 0;
    }

    const float clipped = std::clamp(value, -1.0F, 1.0F);
    return static_cast<std::int16_t>(std::lround(clipped * full_scale));
}

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
