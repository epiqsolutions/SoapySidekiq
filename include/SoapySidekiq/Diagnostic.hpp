/**
 * @file Diagnostic.hpp
 * @brief Contains diagnostic exceptions at callbacks and stream-status boundaries.
 */
#pragma once
#include <utility>

namespace soapy_sidekiq
{
/**
 * Invoke a synchronous diagnostic without letting its exceptions replace a result.
 * @param emit Logger invocation; called once without copying its captured state.
 * Use this when cleanup, backend status, or an accepted sample prefix must survive
 * a failing user-installed log sink. This does not suppress backend operation errors.
 */
template <typename Emit> void reportDiagnostic(Emit &&emit) noexcept
{
    try
    {
        std::forward<Emit>(emit)();
    }
    catch (...)
    {
        // Diagnostics are best effort; retrying a failing sink could recurse.
    }
}
} // namespace soapy_sidekiq
