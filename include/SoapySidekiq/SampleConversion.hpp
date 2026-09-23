#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace soapy_sidekiq
{

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
