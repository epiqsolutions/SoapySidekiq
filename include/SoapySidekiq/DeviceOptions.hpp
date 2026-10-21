/**
 * @file DeviceOptions.hpp
 * @brief Declares validated, hardware-independent device constructor options.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace soapy_sidekiq
{

/** Default Sidekiq TX payload size used when no constructor override is given. */
inline constexpr std::uint32_t defaultTxBlockSize = 16380;

/** Preserve the legacy pool by default while bounding constructor overrides. */
inline constexpr std::uint32_t defaultTxBufferCount = 30000;
/** Largest accepted pool count, bounding payload storage and completion bookkeeping. */
inline constexpr std::uint32_t maxTxBufferCount = 30000;

/** SDK packet limits for the driver's unpacked, single-channel TX payload. */
inline constexpr std::uint32_t txHeaderWords = 4;
/** Required packet-size increment, including the SDK TX header, in words. */
inline constexpr std::uint32_t txPacketIncrementWords = 256;
/** Maximum unpacked complex-sample payload after reserving SDK TX header words. */
inline constexpr std::uint32_t maxTxBlockSize = 65536 - txHeaderWords;

/** Validated constructor settings used to initialize one device. */
struct DeviceOptions
{
    /** Required SDK card index. */
    std::uint8_t card{};
    /** Optional SDK topology identifier. */
    std::optional<std::uint8_t> topology;
    /** TX block size in complex samples. */
    std::uint32_t tx_block_size{defaultTxBlockSize};
    /** Number of preallocated host TX blocks, independent of payload size. */
    std::uint32_t tx_buffer_count{defaultTxBufferCount};
    /** Optional Soapy clock-source name. */
    std::optional<std::string> clock_source;
    /** Optional Soapy time-source name. */
    std::optional<std::string> time_source;
};

/**
 * Parse and range-check Soapy constructor arguments before hardware is opened.
 *
 * @param arguments SoapySDR keyword arguments supplied to the constructor.
 * @return Fully validated options with defaults applied.
 * @throws std::invalid_argument for missing, malformed, empty, or out-of-range
 * values.
 */
DeviceOptions parseDeviceOptions(const std::map<std::string, std::string> &arguments);

} // namespace soapy_sidekiq
