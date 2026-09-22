#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class FakeSidekiqDevice
{
public:
    struct Card
    {
        std::uint8_t id{};
        std::string serial;
        bool available{true};
        std::size_t rx_channels{1};
        std::size_t tx_channels{1};
        std::uint8_t iq_resolution{12};
    };

    struct RxBlock
    {
        std::uint64_t rf_timestamp{};
        std::uint64_t system_timestamp{};
        std::vector<std::int16_t> samples;
    };

    enum class RxResult
    {
        success,
        no_data,
        overrun,
        error
    };

    enum class TxResult
    {
        accepted,
        queue_full,
        error
    };

    using TxCompletion = std::function<void(std::size_t, int)>;

    void addCard(Card card)
    {
        cards_.push_back(std::move(card));
    }

    const std::vector<Card> &cards() const noexcept
    {
        return cards_;
    }

    const Card *findCardById(const std::uint8_t id) const noexcept
    {
        for (const auto &card : cards_)
        {
            if (card.id == id)
            {
                return &card;
            }
        }
        return nullptr;
    }

    const Card *findCardBySerial(const std::string &serial) const noexcept
    {
        for (const auto &card : cards_)
        {
            if (card.serial == serial)
            {
                return &card;
            }
        }
        return nullptr;
    }

    bool initialize(const std::uint8_t id)
    {
        const Card *card = findCardById(id);
        if (card == nullptr || !card->available || initialized_card_.has_value())
        {
            return false;
        }
        initialized_card_ = id;
        return true;
    }

    void shutdown() noexcept
    {
        initialized_card_.reset();
        pending_tx_.clear();
    }

    std::optional<std::uint8_t> initializedCard() const noexcept
    {
        return initialized_card_;
    }

    void enqueueRxBlock(RxBlock block)
    {
        rx_events_.push_back({RxResult::success, std::move(block)});
    }

    void enqueueRxResult(const RxResult result)
    {
        if (result == RxResult::success)
        {
            throw std::invalid_argument("success requires an RX block");
        }
        rx_events_.push_back({result, std::nullopt});
    }

    std::pair<RxResult, std::optional<RxBlock>> receive()
    {
        requireInitialized();
        if (rx_events_.empty())
        {
            return {RxResult::no_data, std::nullopt};
        }

        auto event = std::move(rx_events_.front());
        rx_events_.pop_front();
        return event;
    }

    void enqueueTxResult(const TxResult result)
    {
        tx_results_.push_back(result);
    }

    TxResult transmit(const std::size_t buffer_index, TxCompletion completion)
    {
        requireInitialized();
        transmitted_indices_.push_back(buffer_index);
        const TxResult result = tx_results_.empty()
            ? TxResult::accepted
            : popTxResult();
        if (result == TxResult::accepted)
        {
            pending_tx_.push_back({buffer_index, std::move(completion)});
        }
        return result;
    }

    void completeTx(const std::size_t buffer_index, const int status = 0)
    {
        for (auto pending = pending_tx_.begin(); pending != pending_tx_.end(); ++pending)
        {
            if (pending->buffer_index == buffer_index)
            {
                auto completion = std::move(pending->completion);
                pending_tx_.erase(pending);
                completion(buffer_index, status);
                return;
            }
        }
        throw std::logic_error("fake device has no matching pending TX buffer");
    }

    std::size_t pendingTxCount() const noexcept
    {
        return pending_tx_.size();
    }

    const std::vector<std::size_t> &transmittedIndices() const noexcept
    {
        return transmitted_indices_;
    }

private:
    struct PendingTx
    {
        std::size_t buffer_index;
        TxCompletion completion;
    };

    void requireInitialized() const
    {
        if (!initialized_card_.has_value())
        {
            throw std::logic_error("fake device is not initialized");
        }
    }

    TxResult popTxResult()
    {
        const TxResult result = tx_results_.front();
        tx_results_.pop_front();
        return result;
    }

    std::vector<Card> cards_;
    std::optional<std::uint8_t> initialized_card_;
    std::deque<std::pair<RxResult, std::optional<RxBlock>>> rx_events_;
    std::deque<TxResult> tx_results_;
    std::vector<PendingTx> pending_tx_;
    std::vector<std::size_t> transmitted_indices_;
};
