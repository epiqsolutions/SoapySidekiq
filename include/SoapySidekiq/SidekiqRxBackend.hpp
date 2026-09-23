#pragma once

#include <SoapySidekiq/RxStreamSession.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

/** Adapts the Sidekiq C receive API to the testable RxStreamBackend contract. */
class SidekiqRxBackend final : public RxStreamBackend
{
public:
    /** Bind the backend to one initialized Sidekiq card. */
    explicit SidekiqRxBackend(std::uint8_t card);

    /** Start the requested Sidekiq RX handle. */
    int start(std::uint32_t handle, bool on_pps) override;

    /** Stop the requested Sidekiq RX handle. */
    int stop(std::uint32_t handle, bool on_pps) override;

    /** Convert one SDK receive result and block into portable value types. */
    RxReceiveResult receive() override;

private:
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
