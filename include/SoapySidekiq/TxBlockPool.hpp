/**
 * @file TxBlockPool.hpp
 * @brief Owns TX blocks while permitting accepted sends to retain individual blocks.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace soapy_sidekiq
{
/**
 * Preallocated block owners shared with accepted-send contexts.
 *
 * Allocation and release are injected so the same ownership path can use SDK
 * allocation in the module and a tracked allocator in non-hardware tests.
 * Destroying the pool releases unused blocks immediately. A retained block is
 * released only after its last send context and pool reference are gone.
 * Pool construction rolls back every allocated block on failure.
 *
 * Allocation/release operate on memory only; they must not depend on a device
 * remaining alive. Release must not throw. The pool itself is immutable after
 * construction; callers serialize payload access with their slot reservations.
 */
template <typename Block> class TxBlockPool
{
  public:
    /** Allocate count blocks of samples complex samples each, or unwind completely. */
    template <typename Allocate, typename Release>
    TxBlockPool(std::size_t count, std::uint32_t samples, Allocate allocate, Release release)
    {
        if (count == 0 || samples == 0)
            throw std::invalid_argument("TX block count and sample count must be positive");
        blocks_.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::unique_ptr<Block, Release> owner(allocate(samples), release);
            if (!owner)
                throw std::bad_alloc();
            // A failing shared-owner allocation leaves owner responsible for release.
            blocks_.emplace_back(std::move(owner));
        }
    }

    /** Retain one reserved block until the corresponding submission completes. */
    std::shared_ptr<Block> block(std::size_t index) const
    {
        return blocks_.at(index);
    }

  private:
    /** Immutable pool references; accepted contexts independently retain their blocks. */
    std::vector<std::shared_ptr<Block>> blocks_;
};
} // namespace soapy_sidekiq
