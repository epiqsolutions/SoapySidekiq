#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace soapy_sidekiq
{

template <typename RxHandle, typename TxHandle>
class ChannelMap
{
public:
    ChannelMap() = default;

    ChannelMap(std::vector<RxHandle> rx_handles, std::vector<TxHandle> tx_handles)
        : rx_handles_(std::move(rx_handles)),
          tx_handles_(std::move(tx_handles))
    {
    }

    RxHandle rxHandle(const std::size_t channel) const
    {
        if (channel >= rx_handles_.size())
        {
            throw std::out_of_range("invalid RX channel " + std::to_string(channel));
        }
        return rx_handles_[channel];
    }

    TxHandle txHandle(const std::size_t channel) const
    {
        if (channel >= tx_handles_.size())
        {
            throw std::out_of_range("invalid TX channel " + std::to_string(channel));
        }
        return tx_handles_[channel];
    }

    std::size_t rxCount() const noexcept
    {
        return rx_handles_.size();
    }

    std::size_t txCount() const noexcept
    {
        return tx_handles_.size();
    }

private:
    std::vector<RxHandle> rx_handles_;
    std::vector<TxHandle> tx_handles_;
};

} // namespace soapy_sidekiq
