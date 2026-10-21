/**
 * @file ScopeExit.hpp
 * @brief Releases acquired resources when construction exits exceptionally.
 */

#pragma once

#include <type_traits>
#include <utility>

namespace soapy_sidekiq
{

/** Own a nonthrowing cleanup callback until ownership is explicitly released. */
template <typename Cleanup> class ScopeExit
{
  public:
    /** Arm cleanup without allocating memory after resource acquisition. */
    explicit ScopeExit(Cleanup cleanup) noexcept : cleanup_(std::move(cleanup))
    {
        static_assert(std::is_nothrow_move_constructible_v<Cleanup>);
        static_assert(std::is_nothrow_invocable_v<Cleanup>);
    }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

    /** Release resources on normal or exceptional scope exit while armed. */
    ~ScopeExit() noexcept
    {
        if (armed_)
            cleanup_();
    }

    /** Transfer responsibility for cleanup to the successfully built device. */
    void release() noexcept
    {
        armed_ = false;
    }

  private:
    /** Nonthrowing resource release action, owned without dynamic allocation. */
    Cleanup cleanup_;
    /** Invoke cleanup on scope exit until responsibility is explicitly released. */
    bool armed_{true};
};

} // namespace soapy_sidekiq
