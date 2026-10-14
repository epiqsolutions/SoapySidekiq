/** @file SampleScale.hpp
 * @brief Derives native signed sample scales from SDK ADC/DAC widths.
 */
#pragma once

#include <cstdint>
#include <stdexcept>

namespace soapy_sidekiq
{
/** Return the positive full scale for a signed sample stored in int16_t.
 * @throws std::invalid_argument for widths outside the supported 2..16 range.
 */
inline std::uint32_t sampleFullScale(const std::uint8_t resolution)
{
    if (resolution < 2 || resolution > 16)
        throw std::invalid_argument("sample resolution must be between 2 and 16 bits");
    return (std::uint32_t{1} << (resolution - 1)) - 1;
}
}
