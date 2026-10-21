/**
 * @file TxCompletionState.hpp
 * @brief Owns TX completion bookkeeping independently of a device's lifetime.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace soapy_sidekiq
{
/**
 * Shared by a TX stream and each accepted send context.
 *
 * An old stream's callbacks retain its own state after close/reopen or device
 * destruction. They never dereference the device or notify its destroyed locks.
 * The condition type permits wait-boundary injection in the production-method
 * tests; normal code uses std::condition_variable.
 *
 * This owns bookkeeping only; SDK block and SDK/card lifetimes are separate.
 */
template <typename Condition = std::condition_variable> struct TxCompletionState
{
    /** Allocate zero-initialized slot reservations before publishing a stream. */
    explicit TxCompletionState(std::size_t count) : slots(count) {}

    /** Guards slots/completed and the device's write/status admission counters. */
    std::mutex mutex;
    /** Wake writers and status readers after completion or admission changes. */
    Condition changed;
    /** Zero means available; one means reserved or accepted by the SDK. */
    std::vector<std::int32_t> slots;
    /** Monotonic completion progress, protected by mutex. */
    std::uint64_t completed{};
};
} // namespace soapy_sidekiq
