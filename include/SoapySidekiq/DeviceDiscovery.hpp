/**
 * @file DeviceDiscovery.hpp
 * @brief Hardware-independent types and interfaces for Sidekiq discovery.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace soapy_sidekiq
{

/** One Sidekiq card returned by discovery. */
struct DeviceDescriptor
{
    /** SDK card index used to open the device. */
    std::uint8_t card{};
    /** Hardware serial string reported by the SDK. */
    std::string serial;
    /** Whether this process can currently access the card. */
    bool available{};
};

/** Optional filters accepted by the discovery routine. */
struct DiscoveryQuery
{
    /** Decimal card index; takes precedence over serial when both are set. */
    std::optional<std::string> card;
    /** Exact serial string to match when no card filter is supplied. */
    std::optional<std::string> serial;
};

/** Recoverable diagnostic collected while enumerating cards. */
struct DiscoveryError
{
    /** Affected card, or no value when enumeration itself failed. */
    std::optional<std::uint8_t> card;
    /** Human-readable name of the failed backend operation. */
    std::string operation;
    /** Backend error detail suitable for driver logging. */
    std::string message;
};

/** Devices and diagnostics returned together by a discovery pass. */
struct DiscoveryResult
{
    /** Successfully described devices that match the requested filters. */
    std::vector<DeviceDescriptor> devices;
    /** Recoverable errors encountered during the same discovery pass. */
    std::vector<DiscoveryError> errors;
};

/** Hardware boundary used by production discovery and non-hardware fakes. */
class DiscoveryBackend
{
public:
    virtual ~DiscoveryBackend() = default;

    /**
     * Return every Sidekiq card index visible to the transport.
     * @return Card indexes in backend-defined enumeration order.
     */
    virtual std::vector<std::uint8_t> cardIds() = 0;

    /**
     * Read the serial string for one card.
     * @param card SDK card index to inspect.
     * @return Hardware serial string reported for the card.
     */
    virtual std::string serial(std::uint8_t card) = 0;

    /**
     * Return whether the current process can access one card.
     * @param card SDK card index to inspect.
     * @return True when the current process may open the card.
     */
    virtual bool isAvailable(std::uint8_t card) = 0;
};

/**
 * Enumerate and filter devices while retaining per-card errors.
 *
 * A serial-read failure excludes that card because it cannot produce a valid
 * descriptor. An availability-read failure retains the card as unavailable.
 *
 * @param backend Hardware adapter used for all discovery operations.
 * @param query Optional card or serial filter.
 * @return Matching descriptors and any recoverable diagnostics.
 */
DiscoveryResult discoverDevices(
    DiscoveryBackend &backend,
    const DiscoveryQuery &query = {});

} // namespace soapy_sidekiq
