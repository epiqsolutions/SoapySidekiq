/** @file SampleScale.hpp
 * @brief Derives signed sample scales from ADC/DAC widths and TX host alignment.
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

/** Position of meaningful DAC bits inside an unpacked signed 16-bit word. */
enum class TxSampleAlignment
{
    right_justified,
    upper_bits
};

/**
 * Return the host TX full scale, accounting for unpacked sample alignment.
 * Upper-aligned DACs consume the high bits of a full-width signed host word;
 * their physical resolution does not reduce the host's amplitude domain.
 * @param resolution Physical DAC width reported by the SDK.
 * @param alignment Position of the meaningful DAC bits in the host word.
 * @return Positive native CS16 host full scale.
 * @throws std::invalid_argument for invalid widths or unknown alignment.
 */
inline std::uint32_t txSampleFullScale(const std::uint8_t resolution,
                                     const TxSampleAlignment alignment)
{
    const auto physical_scale = sampleFullScale(resolution);
    switch (alignment)
    {
        case TxSampleAlignment::right_justified: return physical_scale;
        case TxSampleAlignment::upper_bits: return sampleFullScale(16);
    }
    throw std::invalid_argument("unsupported TX sample alignment");
}
}
