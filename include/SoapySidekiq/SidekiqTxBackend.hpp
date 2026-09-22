#pragma once

#include <SoapySidekiq/TxStreamWriter.hpp>

#include <cstdint>

namespace soapy_sidekiq
{

class SidekiqTxBackend final : public TxStreamBackend
{
public:
    explicit SidekiqTxBackend(std::uint8_t card);
    ~SidekiqTxBackend() override;

    TxSendResult transmit(
        std::uint32_t handle,
        const std::vector<std::int16_t> &samples,
        Completion completion) override;
    int readUnderruns(std::uint32_t handle, std::uint32_t &count) override;
    void shutdown() noexcept;

private:
    std::uint8_t card_;
    bool registered_{};
};

} // namespace soapy_sidekiq
