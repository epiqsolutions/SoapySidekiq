#pragma once

#include <SoapySidekiq/RxStreamSession.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

class SidekiqRxBackend final : public RxStreamBackend
{
public:
    explicit SidekiqRxBackend(std::uint8_t card);

    int start(std::uint32_t handle, bool on_pps) override;
    int stop(std::uint32_t handle, bool on_pps) override;
    RxReceiveResult receive() override;

private:
    std::uint8_t card_;
};

} // namespace soapy_sidekiq
