#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace soapy_sidekiq
{

class AsyncBufferPool
{
public:
    class Lease
    {
    public:
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)),
              index_(other.index_)
        {
        }

        Lease &operator=(Lease &&other) noexcept
        {
            if (this != &other)
            {
                releaseIfOwned();
                pool_ = std::exchange(other.pool_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        ~Lease()
        {
            releaseIfOwned();
        }

        std::size_t index() const noexcept
        {
            return index_;
        }

        void handOff() noexcept
        {
            pool_ = nullptr;
        }

    private:
        friend class AsyncBufferPool;

        Lease(AsyncBufferPool &pool, const std::size_t index) noexcept
            : pool_(&pool), index_(index)
        {
        }

        void releaseIfOwned() noexcept
        {
            if (pool_ != nullptr)
            {
                pool_->release(index_);
                pool_ = nullptr;
            }
        }

        AsyncBufferPool *pool_;
        std::size_t index_;
    };

    explicit AsyncBufferPool(const std::size_t buffer_count)
        : states_(buffer_count, State::available)
    {
        if (buffer_count == 0)
        {
            throw std::invalid_argument("an async buffer pool cannot be empty");
        }
    }

    AsyncBufferPool(const AsyncBufferPool &) = delete;
    AsyncBufferPool &operator=(const AsyncBufferPool &) = delete;

    template <typename Rep, typename Period>
    std::optional<Lease> acquireFor(
        const std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!available_.wait_for(lock, timeout, [this] { return hasAvailableBuffer(); }))
        {
            return std::nullopt;
        }

        for (std::size_t offset = 0; offset < states_.size(); ++offset)
        {
            const std::size_t index = (next_index_ + offset) % states_.size();
            if (states_[index] == State::available)
            {
                states_[index] = State::reserved;
                next_index_ = (index + 1) % states_.size();
                return Lease(*this, index);
            }
        }

        throw std::logic_error("buffer availability predicate was inconsistent");
    }

    void complete(const std::size_t index)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validateIndex(index);
            if (states_[index] != State::reserved)
            {
                throw std::logic_error("completed buffer was not reserved");
            }
            states_[index] = State::available;
        }
        available_.notify_one();
    }

    std::size_t size() const noexcept
    {
        return states_.size();
    }

    std::size_t availableCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const State state : states_)
        {
            if (state == State::available)
            {
                ++count;
            }
        }
        return count;
    }

private:
    enum class State
    {
        available,
        reserved
    };

    bool hasAvailableBuffer() const
    {
        for (const State state : states_)
        {
            if (state == State::available)
            {
                return true;
            }
        }
        return false;
    }

    void validateIndex(const std::size_t index) const
    {
        if (index >= states_.size())
        {
            throw std::out_of_range("async buffer index is out of range");
        }
    }

    void release(const std::size_t index) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            states_[index] = State::available;
        }
        available_.notify_one();
    }

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::vector<State> states_;
    std::size_t next_index_{};
};

} // namespace soapy_sidekiq
