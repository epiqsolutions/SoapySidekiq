#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

class FakeAsyncTransmitter
{
public:
    enum class Result
    {
        accepted,
        queue_full,
        error
    };

    using Completion = std::function<void(std::size_t)>;

    void enqueueResult(const Result result)
    {
        scripted_results_.push_back(result);
    }

    Result transmit(const std::size_t buffer_index, Completion completion)
    {
        transmitted_indices_.push_back(buffer_index);
        const Result result = scripted_results_.empty()
            ? Result::accepted
            : popResult();

        if (result == Result::accepted)
        {
            pending_.push_back({buffer_index, std::move(completion)});
        }
        return result;
    }

    void complete(const std::size_t buffer_index)
    {
        for (auto pending = pending_.begin(); pending != pending_.end(); ++pending)
        {
            if (pending->buffer_index == buffer_index)
            {
                auto completion = std::move(pending->completion);
                pending_.erase(pending);
                completion(buffer_index);
                return;
            }
        }
        throw std::logic_error("fake transmitter has no matching pending buffer");
    }

    std::size_t pendingCount() const noexcept
    {
        return pending_.size();
    }

    const std::vector<std::size_t> &transmittedIndices() const noexcept
    {
        return transmitted_indices_;
    }

private:
    struct Pending
    {
        std::size_t buffer_index;
        Completion completion;
    };

    Result popResult()
    {
        const Result result = scripted_results_.front();
        scripted_results_.pop_front();
        return result;
    }

    std::deque<Result> scripted_results_;
    std::vector<Pending> pending_;
    std::vector<std::size_t> transmitted_indices_;
};
