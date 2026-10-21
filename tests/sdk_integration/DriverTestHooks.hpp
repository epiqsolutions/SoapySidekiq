/**
 * @file DriverTestHooks.hpp
 * @brief Declares test-only observation points without changing device layout.
 */
#pragma once

namespace scripted_sdk
{
enum class DriverTestPoint
{
    writer_pps_wait,
    join_pps_worker
};
/** Record entry only; never modify driver state or block on a test release. */
void driverTestPoint(DriverTestPoint point) noexcept;
/** Wait at most two seconds for a new observation after the supplied count. */
bool waitForDriverPoint(DriverTestPoint point, unsigned previous_count);
} // namespace scripted_sdk
